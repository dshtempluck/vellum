<#
.SYNOPSIS
  Builds the test suite with coverage instrumentation and reports results.

.DESCRIPTION
  Uses clang-cl's source-based coverage, which gives per-line and
  per-region counts rather than the sampled approximation a profiler
  would. The instrumented build is kept in build-coverage/ so the ordinary
  MSVC build is left alone.

  Only the non-UI code is measured: the Win32 windows need a message loop
  and a desktop, and are exercised by tools/screenshot.ps1 instead.

.PARAMETER Html
  Also write an annotated HTML report.

.EXAMPLE
  .\tools\coverage.ps1
  .\tools\coverage.ps1 -Html
#>
param(
  [switch]$Html,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-coverage"

# --- toolchain -------------------------------------------------------------
$llvm = "C:\Program Files\LLVM\bin"
foreach ($tool in @("clang-cl.exe", "llvm-profdata.exe", "llvm-cov.exe")) {
  if (-not (Test-Path (Join-Path $llvm $tool))) {
    throw "$tool not found in $llvm; coverage needs the LLVM toolchain"
  }
}

# clang-cl still needs the MSVC headers, libraries and the Windows SDK.
#
# It also needs a toolset it actually supports: the MSVC standard library
# hard-fails compilation by a Clang older than it expects. So rather than
# taking the first install found, pick the newest toolset this clang can
# still compile against.
$clangMajor = 0
if (((& "$llvm\clang-cl.exe" --version) -join ' ') -match 'version (\d+)') {
  $clangMajor = [int]$matches[1]
}
# MSVC 14.50 and newer demand Clang 20 or newer.
$maxToolset = if ($clangMajor -ge 20) { [version]"99.0" } else { [version]"14.49" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
$chosen = $null
if (Test-Path $vswhere) {
  foreach ($inst in (& $vswhere -all -products * -format value -property installationPath)) {
    $cand = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
    $tools = Join-Path $inst "VC\Tools\MSVC"
    if (-not (Test-Path $cand) -or -not (Test-Path $tools)) { continue }
    foreach ($v in (Get-ChildItem $tools -Directory | Sort-Object Name -Descending)) {
      $ver = $null
      if (-not [version]::TryParse($v.Name, [ref]$ver)) { continue }
      if ($ver -le $maxToolset) {
        if (-not $chosen -or $ver -gt $chosen) { $chosen = $ver; $vcvars = $cand }
        break
      }
    }
  }
}
if (-not $vcvars) {
  throw ("No MSVC toolset found that clang $clangMajor can compile against " +
         "(needs <= $maxToolset). Install the VS 2022 build tools, or a newer LLVM.")
}
Write-Host "Toolset: $chosen" -ForegroundColor DarkGray

# vcvars refuses to re-initialise an environment that is already set up, so
# an inherited one is cleared first. Without this the toolset request is
# ignored and the newest installed headers win, which clang cannot compile.
foreach ($v in @('VSCMD_VER','VSCMD_ARG_TGT_ARCH','INCLUDE','LIB','LIBPATH',
                 'VCToolsInstallDir','VCToolsVersion','VSINSTALLDIR')) {
  Remove-Item -Path ("Env:" + $v) -ErrorAction SilentlyContinue
}
# /d skips any shell autorun, which would otherwise corrupt the capture.
$lines = & cmd.exe /d /s /c "`"$vcvars`" -vcvars_ver=$chosen >nul 2>&1 && set"
foreach ($line in $lines) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
  }
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if ($cmake) { $cmake = $cmake.Source } else {
  $vsdir = Split-Path (Split-Path (Split-Path (Split-Path $vcvars)))
  $cmake = Join-Path $vsdir "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue)
if ($ninja) { $ninja = $ninja.Source } else {
  $vsdir = Split-Path (Split-Path (Split-Path (Split-Path $vcvars)))
  $ninja = Join-Path $vsdir "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
}

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# --- configure and build ---------------------------------------------------
if (-not (Test-Path (Join-Path $build "build.ninja"))) {
  & $cmake -S $root -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=Debug" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_C_COMPILER=$llvm\clang-cl.exe" `
    "-DCMAKE_CXX_COMPILER=$llvm\clang-cl.exe" `
    "-DCMAKE_LINKER=$llvm\lld-link.exe" `
    "-DVELLUM_COVERAGE=ON" `
    "-DPDFIUM_ROOT=$root/third_party/pdfium"
  if ($LASTEXITCODE -ne 0) { throw "coverage configure failed" }
}

& $cmake --build $build --target vellum_tests
if ($LASTEXITCODE -ne 0) { throw "coverage build failed" }

# --- run -------------------------------------------------------------------
$exe = Join-Path $build "vellum_tests.exe"
$raw = Join-Path $build "vellum.profraw"
$data = Join-Path $build "vellum.profdata"
$env:LLVM_PROFILE_FILE = $raw

Push-Location $root            # so the suite finds samples/
try { & $exe } finally { Pop-Location }
$testExit = $LASTEXITCODE
if ($testExit -ne 0) { Write-Warning "tests reported failures (exit $testExit)" }

if (-not (Test-Path $raw)) { throw "no profile written; was the build instrumented?" }
& "$llvm\llvm-profdata.exe" merge -sparse $raw -o $data
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata failed" }

# --- report ----------------------------------------------------------------
# The UI is excluded by path: it is not reachable without a message loop,
# and counting it would only make the number look worse than it means.
$ignore = "third_party|tests|src[\\/]ui[\\/]"

Write-Host ""
& "$llvm\llvm-cov.exe" report $exe "-instr-profile=$data" `
    "-ignore-filename-regex=$ignore"

if ($Html) {
  $out = Join-Path $build "html"
  & "$llvm\llvm-cov.exe" show $exe "-instr-profile=$data" `
      "-ignore-filename-regex=$ignore" `
      "-format=html" "-output-dir=$out" "-show-line-counts-or-regions"
  Write-Host "`nAnnotated report: $out\index.html" -ForegroundColor Green
}

exit $testExit
