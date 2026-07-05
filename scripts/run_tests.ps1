<#
.SYNOPSIS
    Build and run vkm unit tests on Windows (Vulkan and WebGPU backends).

.DESCRIPTION
    Configures, builds, and runs the UnitTests target for the vkm engine.
    Windows runs the Vulkan backend natively, plus the webgpu (Emscripten/WASM) backend
    delegated to scripts/run_tests.py, which builds it via emcmake and executes it
    headlessly in Chrome (requires the emsdk toolchain and a local Chrome/Chromium install;
    skipped with an informational message when either is missing).

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

Write-Host "================================================"
Write-Host "  vkm unit test runner (Windows)"
Write-Host "  Platform   : Windows"
Write-Host "  Build type : $BuildType"
Write-Host "  Build dir  : $BuildDir"
Write-Host "  Jobs       : $Jobs"
Write-Host "  Backends   : vulkan, webgpu"
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

$Results = [ordered]@{}

# --------------------------------------------------------------------------
# Vulkan backend
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "------------------------------------------------"
Write-Host "  Backend: vulkan"
Write-Host "------------------------------------------------"

$VulkanBuildDir = Join-Path $BuildDir "vulkan"
$VulkanFlags    = @(
    "-DVKM_USE_VULKAN_API=ON",
    "-DVKM_USE_METAL_API=OFF",
    "-DBUILD_SAMPLES=OFF"
)

Write-Host "[INFO] Configuring..."
$cmakeArgs = @("-S", $ProjectRoot, "-B", $VulkanBuildDir, "-DCMAKE_BUILD_TYPE=$BuildType") + $VulkanFlags
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] cmake configure failed for vulkan"
    $Results["vulkan"] = "FAIL"
} else {
    Write-Host "[INFO] Building UnitTests..."
    # --config is required for MSVC/Xcode multi-config generators; ignored by single-config ones
    & cmake --build $VulkanBuildDir --target UnitTests --parallel $Jobs --config $BuildType
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] Build failed for vulkan"
        $Results["vulkan"] = "FAIL"
    } else {
        # Locate test binary — cmake puts it in bin/ or directly in the backend build dir
        $VulkanTestBin = Join-Path $VulkanBuildDir "bin\UnitTests.exe"
        if (-not (Test-Path $VulkanTestBin)) {
            # MSVC multi-config places it under <BuildType>/
            $VulkanTestBin = Join-Path $VulkanBuildDir "$BuildType\UnitTests.exe"
        }
        if (-not (Test-Path $VulkanTestBin)) {
            Write-Host "[FAIL] Test binary not found under $VulkanBuildDir"
            $Results["vulkan"] = "FAIL"
        } else {
            Write-Host "[INFO] Running tests..."
            & $VulkanTestBin
            $Results["vulkan"] = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL" }
        }
    }
}

# --------------------------------------------------------------------------
# WebGPU backend (Emscripten/WASM, delegated to run_tests.py's headless Chrome runner)
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "------------------------------------------------"
Write-Host "  Backend: webgpu"
Write-Host "------------------------------------------------"
Write-Host "[INFO] Delegating webgpu backend to scripts/run_tests.py (headless Chrome runner)..."

$DelegateOutput = & python "$ScriptDir\run_tests.py" --backend webgpu --build-dir $BuildDir --build-type $BuildType --no-bootstrap 2>&1 | Out-String
Write-Host $DelegateOutput

if ($DelegateOutput -match "(?m)^RESULT:webgpu:PASS") {
    $Results["webgpu"] = "PASS"
} elseif ($DelegateOutput -match "(?m)^RESULT:webgpu:SKIP") {
    $Results["webgpu"] = "SKIP"
} else {
    $Results["webgpu"] = "FAIL"
}

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "================================================"
Write-Host "  Test Summary"
Write-Host "================================================"
foreach ($BackendName in $Results.Keys) {
    Write-Host ("  {0,-12}  {1}" -f $BackendName, $Results[$BackendName])
}
Write-Host ""

if ($Results.Values -contains "FAIL") {
    exit 1
} else {
    exit 0
}
