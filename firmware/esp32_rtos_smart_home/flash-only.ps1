# Flash-only script - assumes project is already built

Set-Location $PSScriptRoot

Write-Host ""
Write-Host "=== SIMPLE FLASH STEPS ===" -ForegroundColor Cyan
Write-Host "1. Hold BOOT button" -ForegroundColor Yellow
Write-Host "2. Press EN button (while holding BOOT)" -ForegroundColor Yellow
Write-Host "3. Press ENTER here" -ForegroundColor Yellow
Write-Host "4. Wait for 'Writing at...' messages" -ForegroundColor Yellow
Write-Host "5. Release BOOT" -ForegroundColor Yellow
Write-Host "6. Press EN to run your program" -ForegroundColor Green
Write-Host ""
$null = Read-Host "Press ENTER when ready"

Write-Host "Flashing..." -ForegroundColor Cyan
idf.py -p COM5 flash

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "SUCCESS! Now press EN button on ESP32 to run" -ForegroundColor Green
    Write-Host "Starting monitor..." -ForegroundColor Cyan
    Start-Sleep -Seconds 2
    idf.py -p COM5 monitor
} else {
    Write-Host "Flash failed!" -ForegroundColor Red
}
