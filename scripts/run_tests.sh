#!/usr/bin/env bash
# Build and run vkm unit tests for one or more graphics backends.
#
# Usage: ./scripts/run_tests.sh [options]
#   --backend <metal|vulkan|all>   Backend to test (default: platform default)
#   --build-type <Debug|Release>   cmake build type (default: Debug)
#   --build-dir <path>             cmake binary root (default: <project_root>/build)
#   --jobs <n>                     parallel build jobs (default: cpu count)
#   --no-deps-check                skip verifying dependencies/src/ is populated
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --------------------------------------------------------------------------
# Defaults
# --------------------------------------------------------------------------
BACKEND=""
BUILD_TYPE="Debug"
BUILD_DIR="$PROJECT_ROOT/build"
NO_DEPS_CHECK=0

# Detect cpu count
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
        --backend)        BACKEND="$2";    shift 2 ;;
        --build-type)     BUILD_TYPE="$2"; shift 2 ;;
        --build-dir)      BUILD_DIR="$2";  shift 2 ;;
        --jobs)           JOBS="$2";       shift 2 ;;
        --no-deps-check)  NO_DEPS_CHECK=1; shift   ;;
        -h|--help)
            sed -n '2,10p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# --------------------------------------------------------------------------
# Detect platform and resolve default backends
# --------------------------------------------------------------------------
UNAME="$(uname -s)"
case "$UNAME" in
    Darwin)  PLATFORM="macOS"  ;;
    Linux*)  PLATFORM="Linux"  ;;
    CYGWIN*|MINGW*|MSYS*) PLATFORM="Windows" ;;
    *)
        echo "[WARN] Unknown platform: $UNAME — assuming Linux (Vulkan)"
        PLATFORM="Linux"
        ;;
esac

# Determine which backends to attempt for this platform
all_backends_for_platform() {
    case "$PLATFORM" in
        macOS)
            echo "metal"
            # Attempt Vulkan only when VULKAN_SDK is set or vulkaninfo is on PATH
            if [[ -n "${VULKAN_SDK:-}" ]] || command -v vulkaninfo &>/dev/null; then
                echo "vulkan"
            fi
            ;;
        Linux|Windows)
            echo "vulkan"
            ;;
    esac
}

if [[ -z "$BACKEND" ]]; then
    # Default: platform's primary backend (first in the list)
    BACKEND=$(all_backends_for_platform | head -1)
fi

if [[ "$BACKEND" == "all" ]]; then
    BACKENDS=($(all_backends_for_platform))
else
    BACKENDS=("$BACKEND")
fi

# --------------------------------------------------------------------------
# cmake flags per backend
# --------------------------------------------------------------------------
cmake_flags_for_backend() {
    local b="$1"
    case "$b" in
        metal)
            echo "-DVKM_USE_METAL_API=ON -DVKM_USE_VULKAN_API=OFF"
            ;;
        vulkan)
            echo "-DVKM_USE_VULKAN_API=ON -DVKM_USE_METAL_API=OFF"
            ;;
        webgpu)
            echo "SKIP"
            ;;
        *)
            echo "[ERROR] Unknown backend: $b" >&2
            exit 1
            ;;
    esac
}

# --------------------------------------------------------------------------
# Dependency check
# --------------------------------------------------------------------------
if [[ $NO_DEPS_CHECK -eq 0 ]]; then
    DEPS_SRC="$PROJECT_ROOT/dependencies/src"
    if [[ ! -d "$DEPS_SRC" ]]; then
        echo "[ERROR] dependencies/src/ not found at $DEPS_SRC"
        echo "        Run: python3 dependencies/bootstrap.py"
        echo "        Or re-run with --no-deps-check to skip this guard."
        exit 1
    fi
fi

# --------------------------------------------------------------------------
# Banner
# --------------------------------------------------------------------------
echo "================================================"
echo "  vkm unit test runner"
echo "  Platform   : $PLATFORM"
echo "  Build type : $BUILD_TYPE"
echo "  Build dir  : $BUILD_DIR"
echo "  Jobs       : $JOBS"
echo "  Backends   : ${BACKENDS[*]}"
echo "================================================"

# --------------------------------------------------------------------------
# Per-backend build and run
# --------------------------------------------------------------------------
declare -A RESULTS

for b in "${BACKENDS[@]}"; do
    echo ""
    echo "------------------------------------------------"
    echo "  Backend: $b"
    echo "------------------------------------------------"

    FLAGS=$(cmake_flags_for_backend "$b")

    if [[ "$FLAGS" == "SKIP" ]]; then
        echo "[SKIP] $b backend is not yet implemented."
        RESULTS[$b]="SKIP"
        continue
    fi

    BACKEND_BUILD_DIR="$BUILD_DIR/$b"

    # Configure
    echo "[INFO] Configuring..."
    if ! cmake -S "$PROJECT_ROOT" -B "$BACKEND_BUILD_DIR" \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DBUILD_SAMPLES=OFF \
            $FLAGS \
            2>&1; then
        echo "[FAIL] cmake configure failed for $b"
        RESULTS[$b]="FAIL"
        continue
    fi

    # Build
    echo "[INFO] Building UnitTests..."
    if ! cmake --build "$BACKEND_BUILD_DIR" \
            --target UnitTests \
            --parallel "$JOBS" \
            2>&1; then
        echo "[FAIL] Build failed for $b"
        RESULTS[$b]="FAIL"
        continue
    fi

    # Run
    TEST_BIN="$BACKEND_BUILD_DIR/bin/UnitTests"
    if [[ ! -x "$TEST_BIN" ]]; then
        echo "[FAIL] Test binary not found: $TEST_BIN"
        RESULTS[$b]="FAIL"
        continue
    fi

    echo "[INFO] Running tests..."
    if "$TEST_BIN"; then
        RESULTS[$b]="PASS"
    else
        RESULTS[$b]="FAIL"
    fi
done

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo ""
echo "================================================"
echo "  Test Summary"
echo "================================================"

OVERALL=0
for b in "${BACKENDS[@]}"; do
    STATUS="${RESULTS[$b]}"
    printf "  %-12s  %s\n" "$b" "$STATUS"
    if [[ "$STATUS" == "FAIL" ]]; then
        OVERALL=1
    fi
done
echo ""

exit $OVERALL
