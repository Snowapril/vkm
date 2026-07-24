#!/usr/bin/env python3
"""Downloads glTF sample scenes into resources/Scenes/.

Sample assets are tens of megabytes and are not committed to this repository, the same
way third-party sources live under dependencies/ instead of in-tree. Each scene is a
directory in a public sample repository; every file in it is fetched as-is.

    python3 scripts/download_scenes.py                  # every scene below
    python3 scripts/download_scenes.py DamagedHelmet    # just one
    python3 scripts/download_scenes.py --list
"""

import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SCENES_DIR = PROJECT_ROOT / "resources" / "Scenes"

# name -> (repository, directory within it, approximate download size)
SCENES = {
    "DamagedHelmet": ("KhronosGroup/glTF-Sample-Assets", "Models/DamagedHelmet/glTF", "4 MB"),
    "Sponza": ("KhronosGroup/glTF-Sample-Assets", "Models/Sponza/glTF", "53 MB"),
}

GITHUB_API = "https://api.github.com/repos/{repo}/contents/{path}"


def list_scene_files(repo, path):
    """Returns [(filename, download_url)] for one scene directory."""
    request = urllib.request.Request(GITHUB_API.format(repo=repo, path=path),
                                     headers={"Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request) as response:
        entries = json.load(response)

    if not isinstance(entries, list):
        raise RuntimeError(f"Unexpected response for {repo}/{path}: {entries}")
    return [(entry["name"], entry["download_url"]) for entry in entries if entry["type"] == "file"]


def download_scene(name):
    repo, path, size = SCENES[name]
    target_dir = SCENES_DIR / name
    target_dir.mkdir(parents=True, exist_ok=True)

    print(f"--- {name} ({size}) from {repo}/{path}")
    files = list_scene_files(repo, path)

    for index, (filename, url) in enumerate(files, start=1):
        target = target_dir / filename
        if target.exists():
            print(f"    [{index}/{len(files)}] {filename} (already present)")
            continue
        print(f"    [{index}/{len(files)}] {filename}")
        urllib.request.urlretrieve(url, target)

    print(f"--- {name} -> {target_dir}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="*", choices=list(SCENES),
                        help="Scenes to download (default: all)")
    parser.add_argument("--list", action="store_true", help="List the available scenes and exit")
    args = parser.parse_args()

    if args.list:
        for name, (repo, path, size) in SCENES.items():
            print(f"{name:16} {size:>6}  {repo}/{path}")
        return 0

    for name in (args.scenes or list(SCENES)):
        try:
            download_scene(name)
        except (urllib.error.URLError, RuntimeError) as error:
            print(f"[ERROR] Failed to download {name}: {error}", file=sys.stderr)
            return 1

    print("\nRun a scene with:")
    print("  python3 scripts/run_sample.py --sample model_viewer --backend metal")
    print(f"  (override with --gv_model_path=<file>, default: {SCENES_DIR}/DamagedHelmet/DamagedHelmet.gltf)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
