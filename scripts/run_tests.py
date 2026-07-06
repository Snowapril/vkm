#!/usr/bin/env python3
"""Build and run vkm unit tests for all available graphics backends on the current platform.

Platform / backend matrix
  macOS  : metal (always), vulkan (if VULKAN_SDK env var or vulkaninfo found),
           webgpu (if emsdk + Chrome/Chromium found)
  Windows: vulkan, webgpu (if emsdk + Chrome/Chromium found)
  Linux  : vulkan, webgpu (if emsdk + Chrome/Chromium found)

The webgpu backend targets Emscripten/WASM. It's configured/built via emcmake using the
emsdk toolchain bootstrap.py installs into dependencies/src/emsdk, then executed headlessly
in Chrome (Node.js lacks navigator.gpu, so the emdawnwebgpu backend can't run there). When
emsdk or Chrome/Chromium isn't found, it's skipped with an informational message rather
than failing the run.

Usage
-----
  python3 scripts/run_tests.py [--build-type Debug|Release]
                                [--build-dir <path>]
                                [--backend <name>]
                                [--no-bootstrap]
                                [--jobs <n>]
"""

import argparse
import functools
import http.server
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import threading
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


def metal4_available() -> bool:
    """Return True when the active macOS SDK provides the Metal 4 (MTL4*) headers.

    Metal 4 headers only ship starting with the macOS 26 SDK. Older runner images
    (e.g. macOS 14/15 with Xcode 15.x/16.x) lack them entirely, so the metal backend
    build must be skipped there rather than hard-failing."""
    try:
        result = subprocess.run(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    sdk_path = Path(result.stdout.strip())
    header = sdk_path / "System/Library/Frameworks/Metal.framework/Headers/MTL4CommandBuffer.h"
    return header.exists()


def _emcmake_path(emsdk_dir: Path) -> Path:
    """emcmake lives under emsdk's upstream/emscripten checkout, not the emsdk repo root."""
    name = "emcmake.bat" if platform.system() == "Windows" else "emcmake"
    return emsdk_dir / "upstream" / "emscripten" / name


def emsdk_available():
    """Return the bootstrapped emsdk directory (Path) if usable, else None."""
    emsdk_dir = PROJECT_ROOT / "dependencies" / "src" / "emsdk"
    return emsdk_dir if _emcmake_path(emsdk_dir).exists() else None


def chrome_executable():
    """Return a path to a Chrome/Chromium binary usable for headless WebGPU testing, if found."""
    system = platform.system()
    if system == "Darwin":
        candidates = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
    elif system == "Windows":
        candidates = [
            r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        ]
    else:
        candidates = ["google-chrome", "google-chrome-stable", "chromium", "chromium-browser"]

    for candidate in candidates:
        if os.path.isabs(candidate):
            if os.path.exists(candidate):
                return candidate
        else:
            found = shutil.which(candidate)
            if found:
                return found
    return None


# ---------------------------------------------------------------------------
# Backend discovery
# ---------------------------------------------------------------------------

# Each entry is (backend_name, cmake_flag_dict | None | "WASM").
# None means "not implemented yet" and results in a SKIP with an info message.
# "WASM" routes through run_webgpu_backend() instead of the generic cmake flow below.

def backends_for_platform(system: str) -> list:
    entries = []

    if system == "Darwin":          # macOS — Metal needs the Metal 4 SDK; Vulkan needs MoltenVK
        if metal4_available():
            entries.append(("metal", {
                "VKM_USE_METAL_API":  "ON",
                "VKM_USE_VULKAN_API": "OFF",
            }))
        else:
            print("[INFO] Metal 4 headers (MTL4CommandBuffer.h) not found in the active macOS SDK.")
            print("[INFO] Skipping Metal backend (requires the macOS 26 SDK or newer).")
        if vulkan_available():
            entries.append(("vulkan", {
                "VKM_USE_VULKAN_API": "ON",
                "VKM_USE_METAL_API":  "OFF",
            }))
        else:
            print("[INFO] Vulkan SDK / vulkaninfo not found on this macOS machine.")
            print("[INFO] Skipping Vulkan backend (install MoltenVK or the LunarG Vulkan SDK).")
        entries.append(("webgpu", "WASM"))

    elif system == "Windows":
        entries.append(("vulkan", {
            "VKM_USE_VULKAN_API": "ON",
            "VKM_USE_METAL_API":  "OFF",
        }))
        entries.append(("webgpu", "WASM"))

    else:                           # Linux / Ubuntu
        entries.append(("vulkan", {
            "VKM_USE_VULKAN_API": "ON",
            "VKM_USE_METAL_API":  "OFF",
        }))
        entries.append(("webgpu", "WASM"))

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


def cmake_build(build_dir: Path, build_type: str, jobs: int) -> bool:
    cmd = [
        "cmake",
        "--build", str(build_dir),
        "--target", "UnitTests",
        "--config", build_type,
        "--parallel", str(jobs),
    ]
    result = run_cmd(cmd)
    return result.returncode == 0


def execute_tests(build_dir: Path, build_type: str, system: str) -> str:
    """Runs the built UnitTests binary. Returns 'PASS', 'FAIL', or 'SKIP'.

    A nonzero exit is always FAIL — hardware/driver gaps are handled gracefully
    inside the binary itself (see tests/UnitTestUtils.hpp's VKM_REQUIRE_DEVICE) and
    never produce a nonzero exit, so a real failure here means a genuine regression.
    SKIP applies when the binary exits 0 but printed at least one "Skipping: "
    message (a GPU-dependent test self-skipped due to no compatible hardware).
    """
    binary_name = "UnitTests.exe" if system == "Windows" else "UnitTests"
    # Multi-config generators (e.g. Visual Studio) place binaries under a per-config
    # subdirectory; single-config generators (Makefiles/Ninja) place them directly in bin/.
    candidates = [build_dir / "bin" / build_type / binary_name, build_dir / "bin" / binary_name]
    test_bin = next((path for path in candidates if path.exists()), None)

    if test_bin is None:
        print(f"[ERROR] Test binary not found in any of: {', '.join(str(path) for path in candidates)}")
        return "FAIL"

    print(f">>> {test_bin}")
    result = subprocess.run([str(test_bin)], capture_output=True, text=True)
    print(result.stdout + result.stderr)

    if result.returncode != 0:
        return "FAIL"
    return "SKIP" if "Skipping: " in result.stdout else "PASS"


# ---------------------------------------------------------------------------
# webgpu backend (Emscripten/WASM, executed headlessly in Chrome)
# ---------------------------------------------------------------------------

_ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip_ansi(text: str) -> str:
    """doctest's console reporter colorizes output; strip escape codes before scanning it
    for completion markers, since a code can land between two words (e.g. between
    "[doctest] " and "Status:"), breaking a naive substring search."""
    return _ANSI_ESCAPE_RE.sub("", text)


def _capture_emsdk_env(emsdk_dir: Path) -> dict:
    """Capture the environment variables emsdk_env.sh/.bat sets, once, as a plain dict —
    faithful to the proven `source emsdk_env.sh && emcmake cmake ...` pattern (see
    .github/workflows/wasm.yml), but captured a single time instead of re-invoked per
    subprocess call."""
    merged = dict(os.environ)

    if platform.system() == "Windows":
        env_script = emsdk_dir / "emsdk_env.bat"
        result = subprocess.run(
            ["cmd", "/c", f'call "{env_script}" >nul && set'],
            capture_output=True, text=True,
        )
        for line in result.stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                merged[key] = value
    else:
        env_script = emsdk_dir / "emsdk_env.sh"
        result = subprocess.run(
            ["bash", "-c", f'source "{env_script}" >/dev/null 2>&1 && env -0'],
            capture_output=True,
        )
        for entry in result.stdout.split(b"\x00"):
            if not entry:
                continue
            key, _, value = entry.decode(errors="replace").partition("=")
            if key:
                merged[key] = value

    return merged


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _serve_directory(directory: Path, port: int) -> http.server.ThreadingHTTPServer:
    handler_cls = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler_cls)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def run_webgpu_tests_headless_chrome(chrome: str, build_dir: Path, timeout_s: int = 60) -> bool:
    html_path = build_dir / "bin" / "UnitTests.html"
    if not html_path.exists():
        print(f"[ERROR] {html_path} not found after build.")
        return False

    port = _free_port()
    server = _serve_directory(build_dir / "bin", port)
    try:
        # Headless Chrome does not exit on its own once the page has loaded and run its
        # script, so a real wall-clock timeout is the normal way this call ends — not an
        # error path. --virtual-time-budget was tried and rejected: it accelerates
        # simulated time for JS timers/rAF but does not reliably let the real WebGPU
        # adapter/device IPC round-trip complete, causing the driver-init fixture to hang
        # indefinitely even though it completes in under a second in real time.
        chrome_cmd = [
            chrome,
            "--headless=new",
            "--disable-gpu-sandbox",
            "--enable-unsafe-webgpu",
            "--enable-features=Vulkan",
            "--use-angle=swiftshader",
            "--enable-logging=stderr",
            "--v=1",
            f"http://localhost:{port}/UnitTests.html",
        ]
        print(f">>> {' '.join(chrome_cmd)}  (runs until {timeout_s}s timeout; this is expected)")
        def _as_text(value) -> str:
            if value is None:
                return ""
            return value if isinstance(value, str) else value.decode(errors="replace")

        try:
            result = subprocess.run(chrome_cmd, capture_output=True, text=True, timeout=timeout_s)
            output = _as_text(result.stdout) + _as_text(result.stderr)
        except subprocess.TimeoutExpired as exc:
            output = _as_text(exc.stdout) + _as_text(exc.stderr)
    finally:
        server.shutdown()
        server.server_close()

    output = _strip_ansi(output)

    if "[doctest] Status: SUCCESS!" in output:
        return True
    if "[doctest] Status: FAILURE!" in output:
        print("[FAIL] doctest reported test failures:")
        print(output[-4000:])
        return False
    print(f"[FAIL] doctest completion marker not found within {timeout_s}s (crash or hang?).")
    print(output[-4000:])
    return False


def run_webgpu_backend(build_dir: Path, build_type: str, jobs: int) -> str:
    """Configure, build, and headlessly test the webgpu (Emscripten/WebGPU) backend.
    Returns 'PASS', 'FAIL', or 'SKIP'."""
    emsdk_dir = emsdk_available()
    if emsdk_dir is None:
        print("[INFO] emsdk not found under dependencies/src/emsdk (run bootstrap.py first).")
        print("[SKIP] webgpu backend requires the Emscripten SDK.")
        return "SKIP"

    chrome = chrome_executable()
    if chrome is None:
        print("[INFO] Chrome/Chromium not found; headless WebGPU test execution needs it.")
        print("[SKIP] webgpu backend requires Chrome for headless test execution.")
        return "SKIP"

    env = _capture_emsdk_env(emsdk_dir)

    emcmake = str(_emcmake_path(emsdk_dir))
    configure_cmd = [
        emcmake, "cmake",
        "-S", str(PROJECT_ROOT),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_SAMPLES=OFF",
    ]
    result = run_cmd(configure_cmd, env=env)
    if result.returncode != 0:
        print("[FAIL] CMake configuration failed for webgpu backend.")
        return "FAIL"

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "UnitTests", "--parallel", str(jobs)]
    result = run_cmd(build_cmd, env=env)
    if result.returncode != 0:
        print("[FAIL] Build failed for webgpu backend.")
        return "FAIL"

    return "PASS" if run_webgpu_tests_headless_chrome(chrome, build_dir) else "FAIL"


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
        "--backend", default=None,
        help="Only run this backend (e.g. metal, vulkan, webgpu). Default: all available "
             "for this platform.",
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
    if args.backend:
        backends = [(n, f) for n, f in backends if n == args.backend]
        if not backends:
            print(f"[ERROR] Unknown or unavailable backend for this platform: {args.backend}")
            sys.exit(1)

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

        if cmake_flags == "WASM":
            backend_build_dir = build_base / name
            results[name] = run_webgpu_backend(backend_build_dir, args.build_type, args.jobs)
            # Stable, script-parseable marker for run_tests.sh/.ps1, which delegate to this
            # script for the webgpu backend specifically and need to read PASS/FAIL/SKIP
            # back without relying on exit codes (argparse itself uses exit code 2 for CLI
            # usage errors, so exit codes alone can't safely carry a third SKIP state here).
            print(f"RESULT:{name}:{results[name]}")
            continue

        backend_build_dir = build_base / name

        configured = cmake_configure(backend_build_dir, args.build_type,
                                     cmake_flags, system)
        if not configured:
            print(f"[FAIL] CMake configuration failed for {name} backend.")
            results[name] = "FAIL"
            continue

        built = cmake_build(backend_build_dir, args.build_type, args.jobs)
        if not built:
            print(f"[FAIL] Build failed for {name} backend.")
            results[name] = "FAIL"
            continue

        results[name] = execute_tests(backend_build_dir, args.build_type, system)

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
