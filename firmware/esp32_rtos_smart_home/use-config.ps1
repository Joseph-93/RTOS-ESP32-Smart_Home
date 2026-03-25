<#
.SYNOPSIS
    Switch the active firmware build configuration to a named product profile.

.DESCRIPTION
    Copies components_config.cmake and sdkconfig.defaults from configs/<product>/
    into the project root, replacing the current active build config.
    
    After switching, you MUST do a full clean + rebuild because CMake caches
    component selections and sdkconfig.defaults affects generated sdkconfig.

.PARAMETER Product
    The product name to switch to. Must match a folder under configs/.
    Available products: floating_candle, lcd_lamp

.EXAMPLE
    .\use-config.ps1 floating_candle
    .\use-config.ps1 lcd_lamp

.NOTES
    After running this script:
      1. idf.py fullclean    (or delete the build/ folder manually)
      2. idf.py build
    
    A full clean is required because:
    - CMake caches ENABLE_* variables — stale cache will override new config
    - sdkconfig is generated from sdkconfig.defaults — must regenerate
#>

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$Product
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$configDir = Join-Path $scriptDir "configs\$Product"

if (-not (Test-Path $configDir)) {
    Write-Host "ERROR: No config found for '$Product'" -ForegroundColor Red
    Write-Host ""
    Write-Host "Available configs:" -ForegroundColor Yellow
    Get-ChildItem "$scriptDir\configs" -Directory | ForEach-Object {
        Write-Host "  $($_.Name)" -ForegroundColor Cyan
    }
    exit 1
}

$srcComponents = Join-Path $configDir "components_config.cmake"
$srcSdkconfig  = Join-Path $configDir "sdkconfig.defaults"
$dstComponents = Join-Path $scriptDir "components_config.cmake"
$dstSdkconfig  = Join-Path $scriptDir "sdkconfig.defaults"

if (-not (Test-Path $srcComponents)) {
    Write-Host "ERROR: Missing $srcComponents" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $srcSdkconfig)) {
    Write-Host "ERROR: Missing $srcSdkconfig" -ForegroundColor Red
    exit 1
}

Copy-Item $srcComponents $dstComponents -Force
Copy-Item $srcSdkconfig  $dstSdkconfig  -Force

Write-Host ""
Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║  Config switched to: $Product$((' ' * (22 - $Product.Length)))║" -ForegroundColor Green
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "Active components:" -ForegroundColor Yellow
Select-String "set\(ENABLE_" $dstComponents | ForEach-Object {
    $line = $_.Line.Trim()
    if ($line -match 'ENABLE_(\w+)\s+(\w+)') {
        $name  = $Matches[1].PadRight(20)
        $state = $Matches[2]
        $color = if ($state -eq 'ON') { 'Green' } else { 'DarkGray' }
        Write-Host "  $name $state" -ForegroundColor $color
    }
}
Write-Host ""
Write-Host "⚠  REQUIRED: Run a full clean before building!" -ForegroundColor Yellow
Write-Host "   idf.py fullclean" -ForegroundColor Cyan
Write-Host "   idf.py build" -ForegroundColor Cyan
Write-Host ""
