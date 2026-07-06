#!/usr/bin/env python3
"""Remove locally generated build and dependency artifacts.

By default only removes native CMake build directories (--build-dir and
build-wasm/). Pass --deps to also remove dependencies/ artifacts written by
bootstrap.py (dependencies/src, dependencies/archives, dependencies/snapshots,
dependencies/.bootstrap.json) — these are slow to re-populate since bootstrap.py
re-clones every third-party library, so they're opt-in rather than default.

Usage
-----
  python3 scripts/run_clean.py [--build-dir <path>] [--deps] [--dry-run]
"""

import argparse
import shutil
import sys
from pathlib import Path

SCRIPT_DIR   = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.resolve()


def remove_path(path: Path, dry_run: bool) -> None:
    if not path.exists():
        return
    tag = "[DRY-RUN]" if dry_run else "[INFO]"
    action = "Would remove" if dry_run else "Removing"
    print(f"{tag} {action}: {path}")
    if dry_run:
        return
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Remove locally generated build and dependency artifacts."
    )
    parser.add_argument(
        "--build-dir", default=str(PROJECT_ROOT / "build"),
        help="Native CMake build directory to remove (default: <project_root>/build)",
    )
    parser.add_argument(
        "--deps", action="store_true",
        help="Also remove dependencies/ artifacts written by bootstrap.py",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Print what would be removed without deleting anything",
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = PROJECT_ROOT / build_dir

    targets = [build_dir, PROJECT_ROOT / "build-wasm"]

    if args.deps:
        deps_dir = PROJECT_ROOT / "dependencies"
        targets += [
            deps_dir / "src",
            deps_dir / "archives",
            deps_dir / "snapshots",
            deps_dir / ".bootstrap.json",
        ]

    for target in targets:
        remove_path(target, args.dry_run)


if __name__ == "__main__":
    main()
