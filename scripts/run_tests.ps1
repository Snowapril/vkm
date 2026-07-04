<#
.SYNOPSIS
    Build and run vkm unit tests on Windows (Vulkan backend).

.DESCRIPTION
    Configures, builds, and runs the UnitTests target for the vkm engine.
    Windows supports the Vulkan backend only.

.PARAMETER BuildType
    cmake build type: Debug or Release (default: Debug)

.PARAMETER BuildDir
    cmake binary root directory (default: <project_root>\build)

.PARAMETER Jobs
    Parallel build jobs. 0 = use NUMBER_OF_PROCESSORS (default: 0)

.PARAMETER NoDepsCheck
    Skip checking that dependencies\src\ is populated

.EXAMPLE
    .\scripts\run_tests.ps1
    .\scripts\run_tests.ps1 -BuildType Release
    .\scripts\run_tests.ps1 -BuildDir C:\vkm_build -Jobs 8
#>
param(
    [string]$BuildType   = "Debug",
    [string]$BuildDir    = "",
    [int]   $Jobs        = 0,
    [switch]$NoDepsCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

if ($BuildDir -eq "") {
    $BuildDir = Join-Path $ProjectRoot "build"
}

# Resolve job count
if ($Jobs -le 0) {
    $Jobs = if ($env:NUMBER_OF_PROCESSORS) { [int]$env:NUMBER_OF_PROCESSORS } else { 4 }
}

# Windows: Vulkan only
$Backend          = "vulkan"
$BackendBuildDir  = Join-Path $BuildDir $Backend
$CmakeFlags       = @(
    "-DVKM_USE_VULKAN_API=ON",
    "-DVKM_USE_METAL_API=OFF",
    "-DBUILD_SAMPLES=OFF"
)

Write-Host "================================================"
Write-Host "  vkm unit test runner (Windows)"
Write-Host "  Platform   : Windows"
Write-Host "  Build type : $BuildType"
Write-Host "  Build dir  : $BuildDir"
Write-Host "  Jobs       : $Jobs"
Write-Host "  Backend    : $Backend"
Write-Host "================================================"

# Dependency check
if (-not $NoDepsCheck) {
    $DepsSrc = Join-Path $ProjectRoot "dependencies\src"
    if (-not (Test-Path $DepsSrc)) {
        Write-Error @"
dependencies\src\ not found at $DepsSrc
Run: python dependencies\bootstrap.py
Or re-run with -NoDepsCheck to skip this guard.
"@
        exit 1
    }
}

Write-Host ""
Write-Host "------------------------------------------------"
Write-Host "  Backend: $Backend"
Write-Host "------------------------------------------------"

# Configure
Write-Host "[INFO] Configuring..."
$cmakeArgs = @(
    "-S", $ProjectRoot,
    "-B", $BackendBuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType"
) + $CmakeFlags

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "[FAIL] cmake configure failed for $Backend"
    exit 1
}

# Build
Write-Host "[INFO] Building UnitTests..."
# --config is required for MSVC/Xcode multi-config generators; ignored by single-config ones
& cmake --build $BackendBuildDir --target UnitTests --parallel $Jobs --config $BuildType
if ($LASTEXITCODE -ne 0) {
    Write-Error "[FAIL] Build failed for $Backend"
    exit 1
}

# Locate test binary — cmake puts it in bin/ or directly in the backend build dir
$TestBin = Join-Path $BackendBuildDir "bin\UnitTests.exe"
if (-not (Test-Path $TestBin)) {
    # MSVC multi-config places it under <BuildType>/
    $TestBin = Join-Path $BackendBuildDir "$BuildType\UnitTests.exe"
}
if (-not (Test-Path $TestBin)) {
    Write-Error "[FAIL] Test binary not found under $BackendBuildDir"
    exit 1
}

# Run
Write-Host "[INFO] Running tests..."
& $TestBin
$TestResult = $LASTEXITCODE

Write-Host ""
Write-Host "================================================"
Write-Host "  Test Summary"
Write-Host "================================================"
if ($TestResult -eq 0) {
    Write-Host ("  {0,-12}  PASS" -f $Backend)
} else {
    Write-Host ("  {0,-12}  FAIL" -f $Backend)
}
Write-Host ""

exit $TestResult
