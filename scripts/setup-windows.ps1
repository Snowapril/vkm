#Requires -Version 5.1
<#
.SYNOPSIS
    Sets up the Vulkan development environment for vkm on Windows.
.DESCRIPTION
    Installs Vulkan SDK, cmake, python, and git via winget, then runs bootstrap.py.
    Requires Windows 10 21H1+ or Windows 11 (winget built-in).
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Split-Path -Parent $ScriptDir

Write-Host "=== vkm Windows Setup ===" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
function Test-Command($cmd) {
    return [bool](Get-Command $cmd -ErrorAction SilentlyContinue)
}

function Install-Via-Winget($id, $displayName) {
    Write-Host "--- Installing $displayName via winget ---"
    winget install --id $id --silent --accept-source-agreements --accept-package-agreements
}

# ---------------------------------------------------------------------------
# winget availability
# ---------------------------------------------------------------------------
if (-not (Test-Command "winget")) {
    Write-Host ""
    Write-Host "ERROR: winget is not available on this system." -ForegroundColor Red
    Write-Host "Please install the following tools manually, then re-run this script:"
    Write-Host "  - Vulkan SDK:  https://vulkan.lunarg.com/sdk/home#windows"
    Write-Host "  - CMake:       https://cmake.org/download/"
    Write-Host "  - Python 3:    https://www.python.org/downloads/"
    Write-Host "  - Git:         https://git-scm.com/download/win"
    exit 1
}

# ---------------------------------------------------------------------------
# Install packages
# ---------------------------------------------------------------------------
Write-Host ""
if (-not (Test-Command "cmake")) {
    Install-Via-Winget "Kitware.CMake" "CMake"
} else {
    Write-Host "cmake already installed, skipping."
}

if (-not (Test-Command "python")) {
    Install-Via-Winget "Python.Python.3.12" "Python 3"
} else {
    Write-Host "python already installed, skipping."
}

if (-not (Test-Command "git")) {
    Install-Via-Winget "Git.Git" "Git"
} else {
    Write-Host "git already installed, skipping."
}

Write-Host ""
Write-Host "--- Installing Vulkan SDK ---"
winget install --id KhronosGroup.VulkanSDK --silent --accept-source-agreements --accept-package-agreements

# ---------------------------------------------------------------------------
# Refresh PATH so newly installed tools are found in this session
# ---------------------------------------------------------------------------
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path", "User")

# ---------------------------------------------------------------------------
# Verify prerequisites
# ---------------------------------------------------------------------------
foreach ($cmd in @("cmake", "python", "git")) {
    if (-not (Test-Command $cmd)) {
        Write-Host "ERROR: '$cmd' still not found after installation." -ForegroundColor Red
        Write-Host "       Please open a new terminal so PATH is refreshed, then re-run."
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Run bootstrap (downloads all source dependencies)
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "--- Running bootstrap ---"
Set-Location $RepoRoot
python bootstrap.py

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Setup complete! ===" -ForegroundColor Green
Write-Host ""
Write-Host "To build:"
Write-Host "  mkdir build; cd build"
Write-Host "  cmake .. -DCMAKE_GENERATOR_PLATFORM=x64 -DCMAKE_BUILD_TYPE=Release"
Write-Host "  cmake --build . --config Release"
