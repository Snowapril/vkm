<#
.SYNOPSIS
    Configure, build, and run a single vkm sample for one graphics backend (Windows).

.DESCRIPTION
    Vulkan is built and run natively. The webgpu (Emscripten/WASM) backend is delegated to
    scripts/run_sample.py, which builds it via emcmake into build-wasm/, serves it locally,
    and opens it in your default browser. Metal is not available on Windows.

.PARAMETER Backend
    Backend to build and run: vulkan or webgpu. (metal is unavailable on Windows.)

.PARAMETER Sample
    Sample name under src\samples\, e.g. "triangle".

.PARAMETER BuildType
    cmake build type: Debug or Release (default: Debug)

.PARAMETER BuildDir
    cmake binary root for native backends (default: <project_root>\build)

.PARAMETER Jobs
    Parallel build jobs. 0 = use NUMBER_OF_PROCESSORS (default: 0)

.EXAMPLE
    .\scripts\run_sample.ps1 -Backend vulkan -Sample triangle
    .\scripts\run_sample.ps1 -Backend webgpu -Sample triangle -BuildType Release
#>
param(
    [Parameter(Mandatory=$true)][string]$Backend,
    [Parameter(Mandatory=$true)][string]$Sample,
    [string]$BuildType = "Debug",
    [string]$BuildDir  = "",
    [int]   $Jobs      = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

if ($BuildDir -eq "") {
    $BuildDir = Join-Path $ProjectRoot "build"
}

if ($Jobs -le 0) {
    $Jobs = if ($env:NUMBER_OF_PROCESSORS) { [int]$env:NUMBER_OF_PROCESSORS } else { 4 }
}

# --------------------------------------------------------------------------
# Sample validation
# --------------------------------------------------------------------------
$SampleDir = Join-Path $ProjectRoot "src\samples\$Sample"
if (-not (Test-Path (Join-Path $SampleDir "CMakeLists.txt"))) {
    Write-Host "[ERROR] Unknown sample: $Sample"
    Write-Host "[ERROR] Available samples:"
    Get-ChildItem (Join-Path $ProjectRoot "src\samples") -Directory | ForEach-Object {
        if (Test-Path (Join-Path $_.FullName "CMakeLists.txt")) {
            Write-Host "  - $($_.Name)"
        }
    }
    exit 1
}

# --------------------------------------------------------------------------
# webgpu: delegate to run_sample.py
# --------------------------------------------------------------------------
if ($Backend -eq "webgpu") {
    Write-Host "[INFO] Delegating webgpu backend to scripts/run_sample.py..."
    & python "$ScriptDir\run_sample.py" --backend webgpu --sample $Sample `
        --build-type $BuildType --jobs $Jobs
    exit $LASTEXITCODE
}

if ($Backend -eq "metal") {
    Write-Error "Metal backend is not available on Windows."
    exit 1
}

if ($Backend -ne "vulkan") {
    Write-Error "Unknown backend: $Backend"
    exit 1
}

# --------------------------------------------------------------------------
# vulkan: build and run natively
# --------------------------------------------------------------------------
$BackendBuildDir = Join-Path $BuildDir "vulkan"
$Flags = @(
    "-DVKM_USE_VULKAN_API=ON",
    "-DVKM_USE_METAL_API=OFF",
    "-DBUILD_SAMPLES=ON"
)

Write-Host "[INFO] Configuring..."
$cmakeArgs = @("-S", $ProjectRoot, "-B", $BackendBuildDir, "-DCMAKE_BUILD_TYPE=$BuildType") + $Flags
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] CMake configuration failed for vulkan backend."
    exit $LASTEXITCODE
}

Write-Host "[INFO] Building $Sample..."
& cmake --build $BackendBuildDir --target $Sample --parallel $Jobs --config $BuildType
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed for vulkan/$Sample."
    exit $LASTEXITCODE
}

# Locate sample binary — cmake puts it in bin/ or directly under <BuildType>/ for MSVC multi-config
$SampleBin = Join-Path $BackendBuildDir "bin\$Sample.exe"
if (-not (Test-Path $SampleBin)) {
    $SampleBin = Join-Path $BackendBuildDir "$BuildType\$Sample.exe"
}
if (-not (Test-Path $SampleBin)) {
    Write-Error "Sample binary not found under $BackendBuildDir"
    exit 1
}

Write-Host "[INFO] Launching $SampleBin..."
& $SampleBin
exit $LASTEXITCODE
