#!/usr/bin/env python3

# Pulls Dawn's third_party/ sources (SPIRV-Tools, SPIRV-Headers, abseil, etc.)
# into the dawn checkout that bootstrap.py just cloned (dependencies/src/dawn).
# Dawn does not vendor these as real git submodules; instead it ships its own
# tools/fetch_dawn_dependencies.py helper that must be run once after cloning,
# otherwise the Tint build targets we need have no sources to compile.
#
# Run from bootstrap.py's working directory (dependencies/), same as any other
# "postprocess: script" entry in bootstrap.json.

import os
import subprocess
import sys


def main():
    dawn_dir = os.path.join("src", "dawn")
    fetch_script = os.path.join(dawn_dir, "tools", "fetch_dawn_dependencies.py")

    command = [sys.executable, fetch_script, "-d", dawn_dir]
    print("--- " + " ".join(command))
    result = subprocess.call(command)
    if result != 0:
        print("ERROR: fetch_dawn_dependencies failed")
        sys.exit(result)


if __name__ == "__main__":
    main()
