#!/usr/bin/env python3
"""Downloads the MediaPipe hand landmarker model and runtime into resources/HandTracking/.

These are ~27 MB of prebuilt binaries and are not committed to this repository, the same way
glTF sample scenes are not (see download_scenes.py) and third-party sources live under
dependencies/. Nothing in the engine requires them: without them the browser hand tracker
refuses to start and its caller falls back to whatever input it has.

Only the browser needs this. Apple platforms track hands through Vision, which ships with the
OS and needs no model file.

    python3 scripts/download_hand_model.py
    python3 scripts/download_hand_model.py --force    # re-download files already present
"""

import argparse
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TARGET_DIR = PROJECT_ROOT / "resources" / "HandTracking"

# Pinned: MediaPipe's wasm runtime and its model files are versioned together, and
# FilesetResolver loads whichever pair sits in the wasm/ directory.
TASKS_VISION_VERSION = "0.10.14"
CDN = f"https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@{TASKS_VISION_VERSION}"
MODELS = "https://storage.googleapis.com/mediapipe-models"

# (url, path relative to TARGET_DIR, approximate size)
FILES = [
    (f"{MODELS}/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task",
     "hand_landmarker.task", "7.8 MB"),
    (f"{CDN}/vision_bundle.mjs", "vision_bundle.mjs", "0.1 MB"),
    # Both the SIMD and the non-SIMD runtime: FilesetResolver picks between them at load time
    # from what the browser reports, so shipping one of the two would break the other browser.
    (f"{CDN}/wasm/vision_wasm_internal.js", "wasm/vision_wasm_internal.js", "0.2 MB"),
    (f"{CDN}/wasm/vision_wasm_internal.wasm", "wasm/vision_wasm_internal.wasm", "9.4 MB"),
    (f"{CDN}/wasm/vision_wasm_nosimd_internal.js", "wasm/vision_wasm_nosimd_internal.js", "0.2 MB"),
    (f"{CDN}/wasm/vision_wasm_nosimd_internal.wasm", "wasm/vision_wasm_nosimd_internal.wasm", "9.3 MB"),
]


def download(url, relative_path, size, force):
    target = TARGET_DIR / relative_path
    if target.exists() and not force:
        print(f"    {relative_path} (already present)")
        return

    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"    {relative_path} ({size})")
    # Downloaded to a temporary name and moved into place, so an interrupted run leaves no
    # half-written file that the next run would take for a complete one.
    partial = target.with_suffix(target.suffix + ".partial")
    urllib.request.urlretrieve(url, partial)
    shutil.move(str(partial), str(target))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true",
                        help="Re-download files that are already present")
    args = parser.parse_args()

    print(f"--- MediaPipe hand landmarker (tasks-vision {TASKS_VISION_VERSION}), ~27 MB total")
    for url, relative_path, size in FILES:
        try:
            download(url, relative_path, size, args.force)
        except (urllib.error.URLError, OSError) as error:
            print(f"[ERROR] Failed to download {relative_path}: {error}", file=sys.stderr)
            return 1

    print(f"--- installed into {TARGET_DIR}")
    print("\nBuild a wasm sample to pick them up (they are copied next to the page at configure time):")
    print("  python3 scripts/run_sample.py --sample hand_interaction --backend webgpu")
    return 0


if __name__ == "__main__":
    sys.exit(main())
