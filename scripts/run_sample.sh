#!/usr/bin/env bash
# Configure, build, and run a single vkm sample for one graphics backend.
#
# Usage: ./scripts/run_sample.sh --backend <metal|vulkan|webgpu> --sample <name> [options] [-- <engine args>]
#   --backend <metal|vulkan|webgpu>  Backend to build and run (required)
#   --sample <name>                  Sample name under src/samples/ (required)
#   --build-type <Debug|Release>     cmake build type (default: Debug)
#   --build-dir <path>               cmake binary root for native backends (default: <project_root>/build)
#   --jobs <n>                       parallel build jobs (default: cpu count)
#   -- <engine args>                 everything after -- is passed to the sample itself,
#                                    e.g. -- --enable-hdr --gv_cpu_profile=1
#
# The webgpu backend is delegated to scripts/run_sample.py, which builds via emcmake into
# build-wasm/ and opens the sample in your default browser.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --------------------------------------------------------------------------
# Defaults
# --------------------------------------------------------------------------
BACKEND=""
SAMPLE=""
BUILD_TYPE="Debug"
BUILD_DIR="$PROJECT_ROOT/build"
ENGINE_ARGS=()

if command -v nproc &>/dev/null; then
    JOBS=$(nproc)
elif command -v sysctl &>/dev/null; then
    JOBS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
else
    JOBS=4
fi

# --------------------------------------------------------------------------
# Parse arguments
# --------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend)    BACKEND="$2";    shift 2 ;;
        --sample)     SAMPLE="$2";     shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --build-dir)  BUILD_DIR="$2";  shift 2 ;;
        --jobs)       JOBS="$2";       shift 2 ;;
        --)
            # Everything past this point belongs to the sample, not to this script, so it is
            # collected verbatim rather than matched against the cases above.
            shift
            ENGINE_ARGS=("$@")
            break
            ;;
        -h|--help)
            sed -n '2,11p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$BACKEND" || -z "$SAMPLE" ]]; then
    echo "[ERROR] --backend and --sample are required." >&2
    exit 1
fi

# --------------------------------------------------------------------------
# Sample validation
# --------------------------------------------------------------------------
if [[ ! -f "$PROJECT_ROOT/src/samples/$SAMPLE/CMakeLists.txt" ]]; then
    echo "[ERROR] Unknown sample: $SAMPLE" >&2
    echo "[ERROR] Available samples:" >&2
    for d in "$PROJECT_ROOT"/src/samples/*/; do
        [[ -f "$d/CMakeLists.txt" ]] && echo "  - $(basename "$d")" >&2
    done
    exit 1
fi

# --------------------------------------------------------------------------
# webgpu: delegate to run_sample.py
# --------------------------------------------------------------------------
if [[ "$BACKEND" == "webgpu" ]]; then
    # The wasm build runs in a browser, so there is no argv to forward to. Say so instead of
    # dropping the arguments silently.
    if [[ ${#ENGINE_ARGS[@]} -gt 0 ]]; then
        echo "[ERROR] Engine arguments cannot be forwarded to the webgpu backend; it runs in a browser." >&2
        exit 1
    fi
    echo "[INFO] Delegating webgpu backend to scripts/run_sample.py..."
    exec python3 "$PROJECT_ROOT/scripts/run_sample.py" \
        --backend webgpu --sample "$SAMPLE" --build-type "$BUILD_TYPE" --jobs "$JOBS"
fi

# --------------------------------------------------------------------------
# metal / vulkan: build and run natively
# --------------------------------------------------------------------------
if [[ "$BACKEND" == "metal" && "$(uname -s)" != "Darwin" ]]; then
    echo "[ERROR] Metal backend is only available on macOS." >&2
    exit 1
fi

case "$BACKEND" in
    metal)  FLAGS="-DVKM_USE_METAL_API=ON -DVKM_USE_VULKAN_API=OFF" ;;
    vulkan) FLAGS="-DVKM_USE_VULKAN_API=ON -DVKM_USE_METAL_API=OFF" ;;
    *)
        echo "[ERROR] Unknown backend: $BACKEND" >&2
        exit 1
        ;;
esac

BACKEND_BUILD_DIR="$BUILD_DIR/$BACKEND"

echo "[INFO] Configuring..."
cmake -S "$PROJECT_ROOT" -B "$BACKEND_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_SAMPLES=ON \
    $FLAGS

echo "[INFO] Building $SAMPLE..."
cmake --build "$BACKEND_BUILD_DIR" --target "$SAMPLE" --parallel "$JOBS"

if [[ "$(uname -s)" == "Darwin" ]]; then
    # Samples are .app bundles on macOS; run the executable inside the bundle so this
    # shell keeps the sample's stdout/stderr and exit code.
    SAMPLE_BIN="$BACKEND_BUILD_DIR/bin/$SAMPLE.app/Contents/MacOS/$SAMPLE"
else
    SAMPLE_BIN="$BACKEND_BUILD_DIR/bin/$SAMPLE"
fi
if [[ ! -x "$SAMPLE_BIN" ]]; then
    echo "[ERROR] Sample binary not found: $SAMPLE_BIN" >&2
    exit 1
fi

echo "[INFO] Launching $SAMPLE_BIN..."
# Guarded rather than expanded unconditionally: under `set -u`, bash 3.2 -- which is what
# /bin/bash still is on macOS -- treats "${ENGINE_ARGS[@]}" on an empty array as unbound.
if [[ ${#ENGINE_ARGS[@]} -gt 0 ]]; then
    exec "$SAMPLE_BIN" "${ENGINE_ARGS[@]}"
fi
exec "$SAMPLE_BIN"
