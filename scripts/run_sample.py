#!/usr/bin/env python3
"""Configure, build, and run a single vkm sample for one graphics backend.

Native backends (metal/vulkan) are built into build/<backend>/ and the resulting
binary is launched directly as a normal GUI process. The webgpu backend is built
via emcmake into build-wasm/ (matching the manual WebGPU instructions in the
README), then served over a local HTTP server and opened in your default browser.

Usage
-----
  python3 scripts/run_sample.py --backend <metal|vulkan|webgpu> --sample <name>
                                 [--build-type Debug|Release]
                                 [--build-dir <path>]
                                 [--jobs <n>]
"""

import argparse
import functools
import http.server
import os
import platform
import shutil
import socket
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path

SCRIPT_DIR   = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.resolve()
SAMPLES_DIR  = PROJECT_ROOT / "src" / "samples"


# ---------------------------------------------------------------------------
# Helpers (copied from run_tests.py to keep the two scripts independent)
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

    Metal 4 headers only ship starting with the macOS 26 SDK. Older Xcode/SDK
    installs lack them entirely, so the metal backend build must be rejected
    with a clear error rather than failing deep inside a cmake/compile error."""
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


# ---------------------------------------------------------------------------
# Sample discovery / validation
# ---------------------------------------------------------------------------

def available_samples() -> list:
    """Sample names discovered by globbing src/samples/*/CMakeLists.txt."""
    return sorted(p.parent.name for p in SAMPLES_DIR.glob("*/CMakeLists.txt"))


def validate_sample(name: str) -> None:
    if not (SAMPLES_DIR / name / "CMakeLists.txt").exists():
        print(f"[ERROR] Unknown sample: {name}")
        samples = available_samples()
        print(f"[ERROR] Available samples: {', '.join(samples) if samples else '(none found)'}")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Backend availability
# ---------------------------------------------------------------------------

NATIVE_BACKEND_FLAGS = {
    "metal":  {"VKM_USE_METAL_API": "ON", "VKM_USE_VULKAN_API": "OFF"},
    "vulkan": {"VKM_USE_VULKAN_API": "ON", "VKM_USE_METAL_API": "OFF"},
}


def check_backend_available(backend: str) -> bool:
    if backend == "metal":
        if platform.system() != "Darwin":
            print("[ERROR] Metal backend is only available on macOS.")
            return False
        if not metal4_available():
            print("[ERROR] Metal 4 headers (MTL4CommandBuffer.h) not found in the active "
                  "macOS SDK. Requires the macOS 26 SDK or newer.")
            return False
        return True
    if backend == "vulkan":
        if not vulkan_available():
            print("[ERROR] Vulkan SDK / vulkaninfo not found. Install the LunarG Vulkan SDK "
                  "(or MoltenVK on macOS) and/or set VULKAN_SDK.")
            return False
        return True
    if backend == "webgpu":
        if emsdk_available() is None:
            print("[ERROR] emsdk not found under dependencies/src/emsdk (run bootstrap.py first).")
            return False
        return True
    return False


# ---------------------------------------------------------------------------
# Build & run
# ---------------------------------------------------------------------------

def cmake_configure_for_sample(build_dir: Path, build_type: str,
                                cmake_flags: dict, system: str) -> bool:
    cmd = [
        "cmake",
        "-S", str(PROJECT_ROOT),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_SAMPLES=ON",
    ]
    for key, value in cmake_flags.items():
        cmd.append(f"-D{key}={value}")

    if system == "Windows":
        cmd += ["-DCMAKE_GENERATOR_PLATFORM=x64"]

    result = run_cmd(cmd)
    return result.returncode == 0


def cmake_build_target(build_dir: Path, target: str, jobs: int, env=None) -> bool:
    cmd = ["cmake", "--build", str(build_dir), "--target", target, "--parallel", str(jobs)]
    result = run_cmd(cmd, env=env)
    return result.returncode == 0


def run_native_sample(backend: str, sample: str, build_type: str,
                       build_dir: Path, jobs: int, system: str) -> int:
    backend_build_dir = build_dir / backend

    if not cmake_configure_for_sample(backend_build_dir, build_type,
                                       NATIVE_BACKEND_FLAGS[backend], system):
        print(f"[ERROR] CMake configuration failed for {backend} backend.")
        return 1

    if not cmake_build_target(backend_build_dir, sample, jobs):
        print(f"[ERROR] Build failed for {backend}/{sample}.")
        return 1

    binary_name = f"{sample}.exe" if system == "Windows" else sample
    binary = backend_build_dir / "bin" / binary_name
    if not binary.exists():
        print(f"[ERROR] Sample binary not found: {binary}")
        return 1

    # Metal has no runtime-togglable validation layer — it's only enabled via the
    # MTL_DEBUG_LAYER env var read at process start.
    env = {**os.environ, "MTL_DEBUG_LAYER": "1"} if backend == "metal" else None

    print(f"[INFO] Launching {binary} ...")
    return run_cmd([str(binary)], env=env).returncode


def run_webgpu_sample(sample: str, build_type: str, jobs: int) -> int:
    emsdk_dir = emsdk_available()
    env = _capture_emsdk_env(emsdk_dir)
    emcmake = str(_emcmake_path(emsdk_dir))
    build_dir = PROJECT_ROOT / "build-wasm"

    configure_cmd = [
        emcmake, "cmake",
        "-S", str(PROJECT_ROOT),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_SAMPLES=ON",
    ]
    if run_cmd(configure_cmd, env=env).returncode != 0:
        print("[ERROR] CMake configuration failed for webgpu backend.")
        return 1

    if not cmake_build_target(build_dir, sample, jobs, env=env):
        print(f"[ERROR] Build failed for webgpu/{sample}.")
        return 1

    html = build_dir / "bin" / f"{sample}.html"
    if not html.exists():
        print(f"[ERROR] {html} not found after build.")
        return 1

    port = _free_port()
    server = _serve_directory(build_dir / "bin", port)
    url = f"http://localhost:{port}/{sample}.html"
    print(f"[INFO] Serving {build_dir / 'bin'} at {url}")
    print("[INFO] Opening in your default browser.")
    webbrowser.open(url)
    try:
        input("[INFO] Press Enter to stop the server and exit...\n")
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Configure, build, and run a single vkm sample for one graphics backend."
    )
    parser.add_argument(
        "--backend", required=True, choices=["metal", "vulkan", "webgpu"],
        help="Graphics backend to build and run the sample with.",
    )
    parser.add_argument(
        "--sample", required=True,
        help="Sample name (subdirectory under src/samples/), e.g. \"triangle\".",
    )
    parser.add_argument(
        "--build-type", default="Debug", choices=["Debug", "Release"],
        help="CMake build type (default: Debug)",
    )
    parser.add_argument(
        "--build-dir", default=str(PROJECT_ROOT / "build"),
        help="Base build output directory for native backends (default: <project_root>/build). "
             "Ignored for --backend webgpu, which always builds into <project_root>/build-wasm.",
    )
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 1,
        help="Parallel build jobs (default: cpu count)",
    )
    args = parser.parse_args()

    validate_sample(args.sample)

    if not check_backend_available(args.backend):
        sys.exit(1)

    system = platform.system()
    if args.backend == "webgpu":
        sys.exit(run_webgpu_sample(args.sample, args.build_type, args.jobs))
    else:
        sys.exit(run_native_sample(args.backend, args.sample, args.build_type,
                                    Path(args.build_dir), args.jobs, system))


if __name__ == "__main__":
    main()
