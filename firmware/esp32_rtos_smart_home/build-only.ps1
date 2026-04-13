# Reliable build script for ESP32.
# Works after idf.py fullclean, after use-config.ps1, or just incrementally.
# Run from the ESP-IDF Terminal.
Set-Location $PSScriptRoot

$lvglCMake = Join-Path $PSScriptRoot "managed_components\lvgl__lvgl\CMakeLists.txt"

# If LVGL isn't present (e.g. after fullclean), run reconfigure first so the
# component manager downloads it. The cmake configure in reconfigure will have
# the wrong link order, but that's fine — we patch it right after.
if (-not (Test-Path $lvglCMake)) {
    Write-Host "Downloading components (post-fullclean)..." -ForegroundColor Cyan
    idf.py reconfigure
    if ($LASTEXITCODE -ne 0) { Write-Host "reconfigure failed" -ForegroundColor Red; exit 1 }
}

# Patch LVGL: remove the circular 'REQUIRES main' that breaks link order in IDF v5.x
if (Test-Path $lvglCMake) {
    $content = [System.IO.File]::ReadAllText($lvglCMake)
    if ($content -match 'REQUIRES main') {
        [System.IO.File]::WriteAllText($lvglCMake, ($content -replace 'REQUIRES main', ''))
        Write-Host "Patched LVGL: removed 'REQUIRES main'" -ForegroundColor Green
    }
}

# Build. If LVGL was just patched, cmake detects the file change, reruns
# configure with the correct link order, then compiles and links cleanly.
idf.py build
