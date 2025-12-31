# Quick build and flash script for ESP32
# Run this from the ESP-IDF Terminal or after running: C:\Espressif\frameworks\v5.5\esp-idf\export.ps1

Set-Location $PSScriptRoot

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
