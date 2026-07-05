#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== vkm macOS Setup ==="
echo "    MoltenVK (Vulkan on Metal) will be installed via Homebrew."

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "ERROR: '$1' not found. Please install it and re-run this script."
        exit 1
    fi
}

# Xcode Command Line Tools are required (provides clang, git, make)
if ! xcode-select -p &>/dev/null; then
    echo "--- Installing Xcode Command Line Tools ---"
    xcode-select --install
    echo "Follow the installer prompt, then re-run this script."
    exit 1
fi

# ---------------------------------------------------------------------------
# Homebrew
# ---------------------------------------------------------------------------
if ! command -v brew &>/dev/null; then
    echo ""
    echo "--- Installing Homebrew ---"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

    # Add Homebrew to PATH for Apple Silicon if needed
    if [[ -f /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
fi

echo ""
echo "--- Updating Homebrew ---"
brew update --quiet

# ---------------------------------------------------------------------------
# Install packages
# ---------------------------------------------------------------------------
echo ""
echo "--- Installing cmake, python, git ---"
brew install cmake python git

echo ""
echo "--- Installing MoltenVK + Vulkan headers ---"
# molten-vk provides the Vulkan-on-Metal ICD for macOS
# vulkan-headers provides the Vulkan API headers
# vulkan-loader provides libvulkan.dylib, dynamically loaded by volk at runtime
# vulkan-tools provides vulkaninfo, used to detect the Vulkan backend at build time
brew install molten-vk vulkan-headers vulkan-loader vulkan-tools

# ---------------------------------------------------------------------------
# Verify prerequisites
# ---------------------------------------------------------------------------
check_cmd cmake
check_cmd git
check_cmd python3

# ---------------------------------------------------------------------------
# Run bootstrap (downloads all source dependencies)
# ---------------------------------------------------------------------------
echo ""
echo "--- Running bootstrap ---"
cd "$REPO_ROOT"
python3 bootstrap.py

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== Setup complete! ==="
echo ""
echo "To build:"
echo "  mkdir -p build && cd build"
echo "  cmake .. -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build . --parallel"
