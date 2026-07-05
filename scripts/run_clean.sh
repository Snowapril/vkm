#!/usr/bin/env bash
# Remove locally generated build and dependency artifacts.
#
# Usage: ./scripts/run_clean.sh [options]
#   --build-dir <path>   cmake binary root to remove (default: <project_root>/build)
#   --deps               also remove dependencies/ artifacts written by bootstrap.py
#   --dry-run            print what would be removed without deleting anything
#
# By default only build/ and build-wasm/ are removed. --deps additionally removes
# dependencies/src, dependencies/archives, dependencies/snapshots, and
# dependencies/.bootstrap.json — these are slow to re-populate since bootstrap.py
# re-clones every third-party library, so they're opt-in rather than default.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$PROJECT_ROOT/build"
CLEAN_DEPS=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --deps)       CLEAN_DEPS=1;   shift   ;;
        --dry-run)    DRY_RUN=1;      shift   ;;
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

TARGETS=("$BUILD_DIR" "$PROJECT_ROOT/build-wasm")

if [[ $CLEAN_DEPS -eq 1 ]]; then
    TARGETS+=(
        "$PROJECT_ROOT/dependencies/src"
        "$PROJECT_ROOT/dependencies/archives"
        "$PROJECT_ROOT/dependencies/snapshots"
        "$PROJECT_ROOT/dependencies/.bootstrap.json"
    )
fi

for target in "${TARGETS[@]}"; do
    if [[ ! -e "$target" ]]; then
        continue
    fi
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "[DRY-RUN] Would remove: $target"
    else
        echo "[INFO] Removing: $target"
        rm -rf "$target"
    fi
done
