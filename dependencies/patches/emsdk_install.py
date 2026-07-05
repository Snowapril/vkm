#!/usr/bin/env python3

# Installs and activates the pinned Emscripten SDK release inside the emsdk
# checkout that bootstrap.py just cloned (dependencies/src/emsdk). This is a
# separate step from cloning the emsdk manager repo itself: emsdk's own git
# history is not versioned per-SDK-release, so pinning happens via the
# `emsdk install <version>` command instead of a git revision.
#
# Run from bootstrap.py's working directory (dependencies/), same as any
# other "postprocess: script" entry in bootstrap.json.

import os
import platform
import subprocess
import sys

EMSDK_VERSION = "6.0.2"


def main():
    emsdk_dir = os.path.join("src", "emsdk")
    is_windows = platform.system() == "Windows"
    emsdk_tool = os.path.join(emsdk_dir, "emsdk.bat" if is_windows else "emsdk")

    for args in (["install", EMSDK_VERSION], ["activate", EMSDK_VERSION]):
        command = [emsdk_tool] + args
        print("--- " + " ".join(command))
        result = subprocess.call(command, shell=is_windows)
        if result != 0:
            print("ERROR: emsdk " + args[0] + " " + EMSDK_VERSION + " failed")
            sys.exit(result)


if __name__ == "__main__":
    main()
