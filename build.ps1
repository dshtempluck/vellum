<#
.SYNOPSIS
  Configures and builds vellum with MSVC.

.DESCRIPTION
  Locates a Visual Studio build environment, imports it, then runs CMake
  with Ninja. Pass -Target to build a single target.

.EXAMPLE
  .\build.ps1
  .\build.ps1 -Target vellum_probe -Config Debug
#>
param(
  [string]$Config = "RelWithDebInfo",
  [string]$Target = "",
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

# --- locate a toolchain ----------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
  foreach ($inst in (& $vswhere -all -products * -format value -property installationPath)) {
    $cand = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $cand) { $vcvars = $cand; break }
  }
}
if (-not $vcvars) { throw "No Visual Studio x64 build environment found." }

# --- import its environment into this session ------------------------------
# vcvars only exports to its own cmd process, so capture and replay it.
$lines = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $lines) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
  }
}
Write-Host "Toolchain: $vcvars" -ForegroundColor DarkGray

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $cmake) {
  $vsdir = Split-Path (Split-Path (Split-Path (Split-Path $vcvars)))
  $cmake = Join-Path $vsdir "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if (-not (Test-Path $cmake)) { throw "cmake not found" }
} else { $cmake = $cmake.Source }

$ninja = (Get-Command ninja -ErrorAction SilentlyContinue)
if ($ninja) { $ninja = $ninja.Source } else {
  $vsdir = Split-Path (Split-Path (Split-Path (Split-Path $vcvars)))
  $ninja = Join-Path $vsdir "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
}

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# --- configure -------------------------------------------------------------
if (-not (Test-Path (Join-Path $build "build.ninja"))) {
  & $cmake -S $root -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DPDFIUM_ROOT=$root/third_party/pdfium"
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

# --- build -----------------------------------------------------------------
$args = @("--build", $build)
if ($Target) { $args += @("--target", $Target) }
& $cmake @args
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "`nBuilt into $build" -ForegroundColor Green
Get-ChildItem $build -Filter *.exe | ForEach-Object { "  $($_.Name)" }
