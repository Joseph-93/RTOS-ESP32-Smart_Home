# Build and flash script for ESP32.
# Works after idf.py fullclean, after use-config.ps1, or just incrementally.
# Run from the ESP-IDF Terminal.
Set-Location $PSScriptRoot

$lvglCMake = Join-Path $PSScriptRoot "managed_components\lvgl__lvgl\CMakeLists.txt"

if (-not (Test-Path $lvglCMake)) {
    Write-Host "Downloading components (post-fullclean)..." -ForegroundColor Cyan
    idf.py reconfigure
    if ($LASTEXITCODE -ne 0) { Write-Host "reconfigure failed" -ForegroundColor Red; exit 1 }
}

if (Test-Path $lvglCMake) {
    $content = [System.IO.File]::ReadAllText($lvglCMake)
    if ($content -match 'REQUIRES main') {
        [System.IO.File]::WriteAllText($lvglCMake, ($content -replace 'REQUIRES main', ''))
        Write-Host "Patched LVGL: removed 'REQUIRES main'" -ForegroundColor Green
    }
}

Write-Host "Building..." -ForegroundColor Cyan
idf.py build

if ($LASTEXITCODE -eq 0) {
    Write-Host "Flashing to ESP32..." -ForegroundColor Cyan
    Write-Host "If flash fails, hold BOOT button, press EN, release BOOT, then retry" -ForegroundColor Yellow
    
    # Try flash with automatic reset
    idf.py -p COM5 flash
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Starting monitor (Ctrl+] to exit)..." -ForegroundColor Green
        idf.py -p COM5 monitor
    } else {
        Write-Host "`nFlash failed! Try manual boot mode:" -ForegroundColor Red
        Write-Host "1. Hold BOOT button" -ForegroundColor Yellow
        Write-Host "2. Press and release EN button" -ForegroundColor Yellow
        Write-Host "3. Release BOOT button" -ForegroundColor Yellow
        Write-Host "4. Run this script again" -ForegroundColor Yellow
    }
} else {
    Write-Host "Build failed!" -ForegroundColor Red
}
