#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Diagnose build environment
.DESCRIPTION
    Check if all prerequisites are properly installed and configured
#>

Write-Host "======================================" -ForegroundColor Cyan
Write-Host " Build Environment Diagnostics" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

$allOk = $true

# Check CMake
Write-Host "`n[*] CMake" -ForegroundColor Yellow
$cmake = cmake --version 2>&1
if ($LASTEXITCODE -eq 0) {
    $version = $cmake[0]
    Write-Host "  ✓ $version" -ForegroundColor Green
} else {
    Write-Host "  ✗ Not found or not in PATH" -ForegroundColor Red
    Write-Host "    Install: https://cmake.org/download/" -ForegroundColor Gray
    $allOk = $false
}

# Check ARM GCC
Write-Host "`n[*] arm-none-eabi-gcc" -ForegroundColor Yellow
$gcc = arm-none-eabi-gcc --version 2>&1
if ($LASTEXITCODE -eq 0) {
    $version = $gcc[0]
    Write-Host "  ✓ $version" -ForegroundColor Green
} else {
    Write-Host "  ✗ Not found or not in PATH" -ForegroundColor Red
    Write-Host "    Install STM32CubeCLT or GNU Arm Embedded Toolchain" -ForegroundColor Gray
    $allOk = $false
}

# Check Ninja
Write-Host "`n[*] Ninja" -ForegroundColor Yellow
$ninja = ninja --version 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "  ✓ Ninja $ninja" -ForegroundColor Green
} else {
    Write-Host "  ✗ Not found or not in PATH" -ForegroundColor Red
    Write-Host "    Install: https://github.com/ninja-build/ninja/releases" -ForegroundColor Gray
    $allOk = $false
}

# Check Python
Write-Host "`n[*] Python" -ForegroundColor Yellow
$python = python --version 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "  ✓ $python" -ForegroundColor Green
} else {
    Write-Host "  ✗ Not found or not in PATH" -ForegroundColor Red
    Write-Host "    Install: https://www.python.org/" -ForegroundColor Gray
    $allOk = $false
}

# Check STLink
Write-Host "`n[*] ST-Link (optional, for flashing)" -ForegroundColor Yellow
$stlinkCmd = Get-Command STM32_Programmer_CLI -ErrorAction SilentlyContinue
if ($stlinkCmd) {
    Write-Host "  ✓ STM32CubeProgrammer found" -ForegroundColor Green
} else {
    Write-Host "  ⚠ STM32CubeProgrammer not found (optional, for flashing)" -ForegroundColor Yellow
}

# Summary
Write-Host "`n======================================" -ForegroundColor Cyan
if ($allOk) {
    Write-Host " Ready to Build" -ForegroundColor Cyan
    Write-Host "======================================" -ForegroundColor Cyan
    Write-Host "Run: .\scripts\build_all.ps1" -ForegroundColor White
} else {
    Write-Host " Setup Required" -ForegroundColor Red
    Write-Host "======================================" -ForegroundColor Cyan
    Write-Host "Please install missing components above" -ForegroundColor Red
}
