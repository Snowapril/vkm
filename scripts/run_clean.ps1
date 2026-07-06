<#
.SYNOPSIS
    Remove locally generated build and dependency artifacts.

.DESCRIPTION
    By default only removes native CMake build directories (-BuildDir and
    build-wasm\). Pass -Deps to also remove dependencies\ artifacts written by
    bootstrap.py (dependencies\src, dependencies\archives, dependencies\snapshots,
    dependencies\.bootstrap.json) — these are slow to re-populate since bootstrap.py
    re-clones every third-party library, so they're opt-in rather than default.

.PARAMETER BuildDir
    cmake binary root to remove (default: <project_root>\build)

.PARAMETER Deps
    Also remove dependencies\ artifacts written by bootstrap.py

.PARAMETER DryRun
    Print what would be removed without deleting anything

.EXAMPLE
    .\scripts\run_clean.ps1
    .\scripts\run_clean.ps1 -Deps -DryRun
#>
param(
    [string]$BuildDir = "",
    [switch]$Deps,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

if ($BuildDir -eq "") {
    $BuildDir = Join-Path $ProjectRoot "build"
}

$Targets = @($BuildDir, (Join-Path $ProjectRoot "build-wasm"))

if ($Deps) {
    $DepsDir = Join-Path $ProjectRoot "dependencies"
    $Targets += @(
        (Join-Path $DepsDir "src"),
        (Join-Path $DepsDir "archives"),
        (Join-Path $DepsDir "snapshots"),
        (Join-Path $DepsDir ".bootstrap.json")
    )
}

foreach ($target in $Targets) {
    if (-not (Test-Path $target)) {
        continue
    }
    if ($DryRun) {
        Write-Host "[DRY-RUN] Would remove: $target"
    } else {
        Write-Host "[INFO] Removing: $target"
        Remove-Item -Path $target -Recurse -Force
    }
}
