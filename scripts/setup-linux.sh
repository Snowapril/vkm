#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== vkm Linux Setup ==="

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "ERROR: '$1' not found. Please install it and re-run this script."
        exit 1
    fi
}

# Detect distro
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO_ID="${ID:-unknown}"
    DISTRO_ID_LIKE="${ID_LIKE:-}"
else
    DISTRO_ID="unknown"
    DISTRO_ID_LIKE=""
fi

is_debian() { [[ "$DISTRO_ID" == "ubuntu" || "$DISTRO_ID" == "debian" || "$DISTRO_ID_LIKE" == *"debian"* ]]; }
is_fedora() { [[ "$DISTRO_ID" == "fedora" || "$DISTRO_ID" == "rhel" || "$DISTRO_ID" == "centos" || "$DISTRO_ID_LIKE" == *"fedora"* ]]; }
is_arch()   { [[ "$DISTRO_ID" == "arch" || "$DISTRO_ID" == "manjaro" || "$DISTRO_ID_LIKE" == *"arch"* ]]; }

# ---------------------------------------------------------------------------
# Install system packages
# ---------------------------------------------------------------------------
echo ""
echo "--- Installing system packages ---"

if is_debian; then
    sudo apt-get update -qq
    sudo apt-get install -y \
        build-essential cmake git python3 wget \
        xorg-dev libgl-dev libxcursor-dev libxi-dev \
        libxinerama-dev libxrandr-dev libxkbcommon-dev \
        libwayland-bin wayland-protocols

    # Install Vulkan SDK from LunarG apt repository (Ubuntu only)
    if [[ "$DISTRO_ID" == "ubuntu" ]]; then
        echo ""
        echo "--- Adding LunarG Vulkan SDK repository ---"
        # Determine Ubuntu codename for the repo URL
        UBUNTU_CODENAME="${VERSION_CODENAME:-$(lsb_release -cs 2>/dev/null || echo jammy)}"
        wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc \
            | sudo tee /etc/apt/trusted.gpg.d/lunarg.asc >/dev/null
        sudo wget -qO "/etc/apt/sources.list.d/lunarg-vulkan-${UBUNTU_CODENAME}.list" \
            "https://packages.lunarg.com/vulkan/lunarg-vulkan-${UBUNTU_CODENAME}.list"
        sudo apt-get update -qq
        sudo apt-get install -y vulkan-sdk || {
            echo "NOTE: vulkan-sdk not available for this Ubuntu release via LunarG repo."
            echo "      Falling back to distro Vulkan packages."
            sudo apt-get install -y libvulkan-dev vulkan-tools vulkan-validationlayers-dev
        }
    else
        # Debian — use distro packages
        sudo apt-get install -y libvulkan-dev vulkan-tools
    fi

elif is_fedora; then
    sudo dnf install -y \
        gcc gcc-c++ cmake git python3 make \
        libX11-devel libXcursor-devel libXi-devel \
        libXinerama-devel libXrandr-devel mesa-libGL-devel \
        vulkan-devel vulkan-tools vulkan-validation-layers-devel

elif is_arch; then
    sudo pacman -Syu --noconfirm \
        base-devel cmake git python \
        libx11 libxcursor libxi libxinerama libxrandr \
        vulkan-devel vulkan-tools vulkan-validation-layers

else
    echo "WARNING: Unsupported distro '$DISTRO_ID'. Install manually:"
    echo "  - cmake, git, python3"
    echo "  - X11/GL dev libraries (xorg-dev, libgl-dev, etc.)"
    echo "  - Vulkan SDK (https://vulkan.lunarg.com/sdk/home)"
fi

# ---------------------------------------------------------------------------
# Verify prerequisites are now available
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
