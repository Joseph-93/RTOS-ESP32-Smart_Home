# OTA Update Script - Build and push firmware to ESP32s wirelessly
# No USB cable needed! ESP32s must already be running OTA-enabled firmware.
#
# Usage:
#   .\ota-update.ps1                  # Build + OTA all connected ESP32s
#   .\ota-update.ps1 -SkipBuild       # OTA with existing build
#   .\ota-update.ps1 -Targets "10.0.0.46","10.0.0.47"  # Specific devices only
#
# Prerequisites:
#   - ESP32s must have been USB-flashed at least once with OTA-enabled firmware
#   - ESP32s must be connected to the same WiFi network as this machine
#   - Python 3 must be available (for the HTTP file server)

param(
    [switch]$SkipBuild,
    [string[]]$Targets,        # Specific ESP32 IPs to update (empty = all discovered)
    [int]$WsPort = 80          # WebSocket port on ESP32s (also used for hub OTA serving)
)

Set-Location $PSScriptRoot
$ErrorActionPreference = "Stop"

$BinFile = "build\esp32_rtos_smart_home.bin"

# Use the central_hub venv Python (has websockets + zeroconf installed)
$HubVenvPython = Join-Path $PSScriptRoot "..\..\central_hub\venv\Scripts\python.exe"
if (-not (Test-Path $HubVenvPython)) {
    Write-Host "Central hub venv not found at: $HubVenvPython" -ForegroundColor Red
    Write-Host "The OTA script needs the central_hub venv for websockets/zeroconf packages." -ForegroundColor Yellow
    Write-Host "Run: cd central_hub; python -m venv venv; .\venv\Scripts\Activate.ps1; pip install -r requirements.txt" -ForegroundColor Yellow
    exit 1
}

# ============================================================================
# Step 1: Build
# ============================================================================
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== STEP 1: Building firmware ===" -ForegroundColor Cyan
    idf.py build

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        exit 1
    }
    Write-Host "Build successful!" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "=== Skipping build (using existing binary) ===" -ForegroundColor Yellow
}

# Verify binary exists
if (-not (Test-Path $BinFile)) {
    Write-Host "Firmware binary not found at: $BinFile" -ForegroundColor Red
    Write-Host "Run a build first (without -SkipBuild)" -ForegroundColor Yellow
    exit 1
}

$binSize = (Get-Item $BinFile).Length
Write-Host "Firmware binary: $BinFile ($([math]::Round($binSize / 1024)) KB)" -ForegroundColor Gray

# ============================================================================
# Step 2: Stage firmware via central hub (serves it on port 80 - already open)
# ============================================================================
$HubDir = Join-Path $PSScriptRoot "..\..\central_hub"
$OtaStagingDir = Join-Path $HubDir "ota"

# Create staging dir if needed
if (-not (Test-Path $OtaStagingDir)) {
    New-Item -ItemType Directory -Path $OtaStagingDir | Out-Null
}

# Copy firmware to hub's OTA staging directory
$stagedBin = Join-Path $OtaStagingDir "esp32_rtos_smart_home.bin"
Copy-Item $BinFile $stagedBin -Force
Write-Host "Firmware staged to: $stagedBin" -ForegroundColor Gray

# Get the hub's IP (same machine - same IP the hub uses to talk to ESP32s)
$localIp = (Get-NetIPAddress -AddressFamily IPv4 | 
    Where-Object { 
        $_.InterfaceAlias -notmatch "Loopback|vEthernet|Hyper-V|WSL|Virtual|VMware|VirtualBox|Bluetooth" -and 
        $_.IPAddress -ne "127.0.0.1" -and
        $_.PrefixOrigin -ne "WellKnown"
    } | 
    Sort-Object { 
        if ($_.InterfaceAlias -match "Wi-Fi|WiFi|Wireless|WLAN") { 0 }
        elseif ($_.InterfaceAlias -match "Ethernet") { 1 }
        else { 2 }
    } |
    Select-Object -First 1).IPAddress

if (-not $localIp) {
    Write-Host "Could not determine local IP address!" -ForegroundColor Red
    exit 1
}

# Firmware is served by the central hub's WebSocket server on port 80 at /ota/
$firmwareUrl = "http://${localIp}:${WsPort}/ota/esp32_rtos_smart_home.bin"
Write-Host ""
Write-Host "=== STEP 2: Staging firmware via central hub ===" -ForegroundColor Cyan
Write-Host "Local IP:     $localIp" -ForegroundColor Gray
Write-Host "Firmware URL: $firmwareUrl" -ForegroundColor Gray
Write-Host "(Served by hub on port $WsPort -- no separate HTTP server needed)" -ForegroundColor DarkGray

# Verify the hub is serving it (HEAD to confirm reachable without downloading 1MB)
try {
    $resp = Invoke-WebRequest -Uri $firmwareUrl -Method Head -TimeoutSec 5 -UseBasicParsing -ErrorAction Stop
    $contentLen = $resp.Headers['Content-Length']
    Write-Host "Hub OTA endpoint reachable OK (Content-Length: $contentLen)" -ForegroundColor Green
} catch {
    Write-Host "⚠ Could not reach hub OTA endpoint: $_" -ForegroundColor Yellow
    Write-Host "  Make sure the central hub is running!" -ForegroundColor Yellow
}

# ============================================================================
# Step 3: Discover ESP32s or use provided targets
# ============================================================================
Write-Host ""
Write-Host "=== STEP 3: Finding ESP32 devices ===" -ForegroundColor Cyan

if ($Targets -and $Targets.Count -gt 0) {
    $deviceIps = $Targets
    Write-Host "Using specified targets: $($deviceIps -join ', ')" -ForegroundColor Gray
} else {
    # Discover via mDNS by querying the central hub, or fall back to direct WebSocket probe
    Write-Host "Scanning for ESP32 devices on the network..." -ForegroundColor Gray
    
    # Quick scan: try common ESP32 IPs by probing WebSocket on port 80
    # We use the Python script below for reliable discovery
    $discoverScript = @"
import asyncio
import json
import socket

async def find_esp32s():
    """Try to connect to WebSocket on discovered/known ESP32s."""
    found = []
    
    # Method 1: Try mDNS discovery
    try:
        from zeroconf import Zeroconf, ServiceBrowser
        import time
        
        class Listener:
            def __init__(self):
                self.devices = []
            def add_service(self, zc, type_, name):
                info = zc.get_service_info(type_, name)
                if info and info.addresses:
                    ip = socket.inet_ntoa(info.addresses[0])
                    self.devices.append(ip)
            def remove_service(self, *args): pass
            def update_service(self, *args): pass
        
        zc = Zeroconf()
        listener = Listener()
        browser = ServiceBrowser(zc, "_ws._tcp.local.", listener)
        await asyncio.sleep(3)
        zc.close()
        found.extend(listener.devices)
    except ImportError:
        pass
    
    # Deduplicate
    found = list(set(found))
    
    # Verify each is actually an ESP32 with OTA support
    verified = []
    for ip in found:
        try:
            import websockets
            async with websockets.connect(f"ws://{ip}:$WsPort/ws", open_timeout=3) as ws:
                await ws.send(json.dumps({"type": "get_components", "id": 1}))
                resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=3))
                components = [c["name"] for c in resp.get("components", [])]
                if "OTA" in components:
                    verified.append(ip)
                    print(f"FOUND:{ip}")
                else:
                    print(f"SKIP:{ip} (no OTA component)")
        except Exception as e:
            print(f"SKIP:{ip} ({e})")
    
asyncio.run(find_esp32s())
"@
    
    $pyResult = $discoverScript | & $HubVenvPython 2>&1
    $deviceIps = @()
    foreach ($line in $pyResult) {
        if ($line -match "^FOUND:(.+)$") {
            $deviceIps += $Matches[1]
        } else {
            Write-Host "  $line" -ForegroundColor Gray
        }
    }
    
    if ($deviceIps.Count -eq 0) {
        Write-Host "No ESP32 devices with OTA support found!" -ForegroundColor Red
        Write-Host "Make sure your ESP32s are:" -ForegroundColor Yellow
        Write-Host "  - Connected to the same WiFi network" -ForegroundColor Yellow
        Write-Host "  - Running OTA-enabled firmware (requires initial USB flash)" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "You can also specify targets manually:" -ForegroundColor Yellow
        Write-Host '  .\ota-update.ps1 -Targets "10.0.0.46"' -ForegroundColor Yellow
        Stop-Job $httpJob; Remove-Job $httpJob -Force
        exit 1
    }
}

Write-Host "Devices to update: $($deviceIps -join ', ')" -ForegroundColor Green

# ============================================================================
# Step 4: Send OTA command to each ESP32
# ============================================================================
Write-Host ""
Write-Host "=== STEP 4: Pushing OTA update ===" -ForegroundColor Cyan

$otaScript = @"
import asyncio
import json
import sys

async def trigger_ota(ips, url, ws_port):
    import websockets
    
    results = {}
    for ip in ips:
        try:
            print(f"  [{ip}] Connecting...")
            async with websockets.connect(f"ws://{ip}:{ws_port}/ws", open_timeout=5) as ws:
                # Send OTA command
                msg = json.dumps({"type": "start_ota", "url": url, "id": 999})
                await ws.send(msg)
                
                # Wait for response
                resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                
                if resp.get("success"):
                    print(f"  [{ip}] OTA started! Device will download, verify, and reboot.")
                    results[ip] = True
                else:
                    error = resp.get("error", "unknown error")
                    print(f"  [{ip}] FAILED: {error}")
                    results[ip] = False
                    
        except Exception as e:
            print(f"  [{ip}] ERROR: {e}")
            results[ip] = False
    
    succeeded = sum(1 for v in results.values() if v)
    failed = sum(1 for v in results.values() if not v)
    print(f"\nResults: {succeeded} started, {failed} failed")
    return all(results.values())

ips = sys.argv[1].split(",")
url = sys.argv[2]
ws_port = int(sys.argv[3])
success = asyncio.run(trigger_ota(ips, url, ws_port))
sys.exit(0 if success else 1)
"@

$ipList = $deviceIps -join ","
$otaResult = $otaScript | & $HubVenvPython - $ipList $firmwareUrl $WsPort 2>&1
foreach ($line in $otaResult) {
    if ($line -match "FAILED|ERROR") {
        Write-Host $line -ForegroundColor Red
    } elseif ($line -match "OTA started") {
        Write-Host $line -ForegroundColor Green
    } else {
        Write-Host $line -ForegroundColor Gray
    }
}

# ============================================================================
# Step 5: Wait for downloads, then clean up HTTP server
# ============================================================================
Write-Host ""
Write-Host "=== Waiting for ESP32s to download firmware ===" -ForegroundColor Cyan
Write-Host "HTTP server will stay up for 60 seconds to ensure all devices finish downloading..." -ForegroundColor Gray
Write-Host "(The ESP32s reboot automatically after downloading + verifying)" -ForegroundColor Gray
Write-Host ""

# Wait with a countdown
for ($i = 60; $i -gt 0; $i--) {
    Write-Host "`r  Waiting $i seconds for ESP32s to finish downloading... (Ctrl+C to stop early) " -NoNewline -ForegroundColor DarkGray
    Start-Sleep -Seconds 1
}
Write-Host ""

# Clean up staged firmware file
Remove-Item $stagedBin -ErrorAction SilentlyContinue
Write-Host "Staging directory cleaned up." -ForegroundColor Gray

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  OTA update complete!" -ForegroundColor Green
Write-Host "  ESP32s will reconnect after rebooting." -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
exit 0
