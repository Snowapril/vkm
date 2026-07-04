#!/usr/bin/env python3
"""Build and run vkm unit tests for all available graphics backends on the current platform.

Platform / backend matrix
  macOS  : metal (always), vulkan (if VULKAN_SDK env var or vulkaninfo found), webgpu (skip)
  Windows: vulkan, webgpu (skip)
  Linux  : vulkan, webgpu (skip)

WebGPU has no CMake option or source implementation yet; it is skipped with an informational
message. When it is added, append an entry to backends_for_platform() mirroring the vulkan one.

Usage
-----
  python3 scripts/run_tests.py [--build-type Debug|Release]
                                [--build-dir <path>]
                                [--no-bootstrap]
                                [--jobs <n>]
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR   = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.resolve()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run_cmd(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    """Print then execute a command. Returns the CompletedProcess."""
    print(f">>> {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, **kwargs)


def vulkan_available() -> bool:
    """Return True when a Vulkan loader / SDK can be found on the system."""
    if os.environ.get("VULKAN_SDK"):
        return True
    return shutil.which("vulkaninfo") is not None


# ---------------------------------------------------------------------------
# Backend discovery
# ---------------------------------------------------------------------------

# Each entry is (backend_name, cmake_flag_dict | None).
# None means "not implemented yet" and results in a SKIP with an info message.

def backends_for_platform(system: str) -> list:
    entries = []

    if system == "Darwin":          # macOS — Metal always available; Vulkan needs MoltenVK
        entries.append(("metal", {
            "VKM_USE_METAL_API":  "ON",
            "VKM_USE_VULKAN_API": "OFF",
        }))
        if vulkan_available():
            entries.append(("vulkan", {
                "VKM_USE_VULKAN_API": "ON",
                "VKM_USE_METAL_API":  "OFF",
            }))
        else:
            print("[INFO] Vulkan SDK / vulkaninfo not found on this macOS machine.")
            print("[INFO] Skipping Vulkan backend (install MoltenVK or the LunarG Vulkan SDK).")
        entries.append(("webgpu", None))

    elif system == "Windows":
        entries.append(("vulkan", {
            "VKM_USE_VULKAN_API": "ON",
            "VKM_USE_METAL_API":  "OFF",
        }))
        entries.append(("webgpu", None))

    else:                           # Linux / Ubuntu
        entries.append(("vulkan", {
            "VKM_USE_VULKAN_API": "ON",
            "VKM_USE_METAL_API":  "OFF",
        }))
        entries.append(("webgpu", None))

    return entries


# ---------------------------------------------------------------------------
# Build & run
# ---------------------------------------------------------------------------

def cmake_configure(build_dir: Path, build_type: str,
                    cmake_flags: dict, system: str) -> bool:
    cmd = [
        "cmake",
        "-S", str(PROJECT_ROOT),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_SAMPLES=OFF",
    ]
    for key, value in cmake_flags.items():
        cmd.append(f"-D{key}={value}")

    # Visual Studio generator requires an explicit platform selection for x64.
    if system == "Windows":
        cmd += ["-DCMAKE_GENERATOR_PLATFORM=x64"]

    result = run_cmd(cmd)
    return result.returncode == 0


def cmake_build(build_dir: Path, jobs: int) -> bool:
    cmd = [
        "cmake",
        "--build", str(build_dir),
        "--target", "UnitTests",
        "--parallel", str(jobs),
    ]
    result = run_cmd(cmd)
    return result.returncode == 0


def execute_tests(build_dir: Path, system: str) -> bool:
    binary_name = "UnitTests.exe" if system == "Windows" else "UnitTests"
    test_bin = build_dir / "bin" / binary_name

    if not test_bin.exists():
        print(f"[ERROR] Test binary not found: {test_bin}")
        return False

    result = run_cmd([str(test_bin)])
    return result.returncode == 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build and run vkm unit tests for all available graphics backends."
    )
    parser.add_argument(
        "--build-type", default="Debug", choices=["Debug", "Release"],
        help="CMake build type (default: Debug)",
    )
    parser.add_argument(
        "--build-dir", default=str(PROJECT_ROOT / "build"),
        help="Base build output directory (default: <project_root>/build)",
    )
    parser.add_argument(
        "--no-bootstrap", action="store_true",
        help="Skip running bootstrap.py (useful in CI where bootstrap ran earlier)",
    )
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 1,
        help="Parallel build jobs (default: cpu count)",
    )
    args = parser.parse_args()

    system     = platform.system()
    build_base = Path(args.build_dir)

    print(f"Platform   : {system} ({platform.machine()})")
    print(f"Build type : {args.build_type}")
    print(f"Build dir  : {build_base}")
    print(f"Jobs       : {args.jobs}")
    print()

    # Bootstrap dependencies if not already done.
    if not args.no_bootstrap:
        result = run_cmd(
            [sys.executable, str(PROJECT_ROOT / "bootstrap.py")],
            cwd=str(PROJECT_ROOT),
        )
        if result.returncode != 0:
            print("[ERROR] bootstrap.py failed. Aborting.")
            sys.exit(1)
        print()

    backends = backends_for_platform(system)
    results  = {}   # backend_name -> "PASS" | "FAIL" | "SKIP"

    for name, cmake_flags in backends:
        print()
        print("=" * 44)
        print(f"  Backend : {name}")
        print("=" * 44)

        if cmake_flags is None:
            print(f"[SKIP] {name} backend is not yet implemented.")
            results[name] = "SKIP"
            continue

        backend_build_dir = build_base / name

        configured = cmake_configure(backend_build_dir, args.build_type,
                                     cmake_flags, system)
        if not configured:
            print(f"[FAIL] CMake configuration failed for {name} backend.")
            results[name] = "FAIL"
            continue

        built = cmake_build(backend_build_dir, args.jobs)
        if not built:
            print(f"[FAIL] Build failed for {name} backend.")
            results[name] = "FAIL"
            continue

        passed = execute_tests(backend_build_dir, system)
        results[name] = "PASS" if passed else "FAIL"

    # Summary
    print()
    print("=" * 44)
    print("  Test Summary")
    print("=" * 44)
    for name, status in results.items():
        print(f"  {name:<12}  {status}")
    print()

    if any(s == "FAIL" for s in results.values()):
        sys.exit(1)


if __name__ == "__main__":
    main()
