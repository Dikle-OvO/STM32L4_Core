#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Full build: Bootloader + Application + Merge
.DESCRIPTION
    1. Build Bootloader (16KB, Release)
    2. Build Application (52KB, Release)
    3. Merge into single firmware binary
.EXAMPLE
    .\scripts\build_all.ps1
    .\scripts\build_all.ps1 -Clean
#>
param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

Write-Host "======================================" -ForegroundColor Cyan
Write-Host " STM32L431 Full Build" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

# --- Step 1: Build Bootloader ---
Write-Host "`n[1/3] Building Bootloader..." -ForegroundColor Yellow
Push-Location "$Root/Bootloader"

if ($Clean -and (Test-Path "build_bl")) {
    Remove-Item -Recurse -Force "build_bl"
}

cmake --preset Release 2>&1 | Out-Null
$result = cmake --build build_bl 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Bootloader build failed!" -ForegroundColor Red
    $result | Write-Host
    Pop-Location
    exit 1
}

$blBin = "build_bl/STM32L431_BL.bin"
$blSize = (Get-Item $blBin).Length
Write-Host "  OK: $blSize bytes" -ForegroundColor Green
Pop-Location

# --- Step 2: Build Application ---
Write-Host "`n[2/3] Building Application..." -ForegroundColor Yellow
Push-Location $Root

if ($Clean -and (Test-Path "build/Release")) {
    Remove-Item -Recurse -Force "build/Release"
}

cmake --preset Release 2>&1 | Out-Null
$result = cmake --build build/Release 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Application build failed!" -ForegroundColor Red
    $result | Write-Host
    Pop-Location
    exit 1
}

$appBin = "build/Release/STM32L431CBT6.bin"
$appSize = (Get-Item $appBin).Length
Write-Host "  OK: $appSize bytes" -ForegroundColor Green
Pop-Location

# --- Step 3: Merge ---
Write-Host "`n[3/3] Merging firmware..." -ForegroundColor Yellow
Push-Location $Root

python scripts/merge_hex.py `
    --bl "Bootloader/$blBin" `
    --app "$appBin" `
    -o "build/merged_firmware.bin"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Merge failed!" -ForegroundColor Red
    Pop-Location
    exit 1
}

$mergedSize = (Get-Item "build/merged_firmware.bin").Length
Pop-Location

Write-Host "`n======================================" -ForegroundColor Cyan
Write-Host " Build Complete" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Bootloader:  $blSize bytes (limit 16384)" -ForegroundColor White
Write-Host "  Application: $appSize bytes (limit 53248)" -ForegroundColor White
Write-Host "  Merged:      $mergedSize bytes" -ForegroundColor White
Write-Host "  Output:      build/merged_firmware.bin" -ForegroundColor White
