#!/usr/bin/env bash
set -euo pipefail
if [[ $(id -u) != 0 ]]; then
    echo 'Run this dependency installer as root on Ubuntu 24.04.' >&2
    exit 1
fi
source /etc/os-release
if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 24.04 ]]; then
    echo 'The supported dedicated server build/runtime platform is Ubuntu 24.04 x86_64.' >&2
    exit 1
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config python3-venv ca-certificates \
    libflac-dev libminizip-dev liblz4-dev libpng-dev libtbb-dev libgl1-mesa-dev libglu1-mesa-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxfixes-dev \
    libxxf86vm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev \
    libegl1-mesa-dev libwayland-dev libxkbcommon-dev git tar gzip
python3 -m venv /opt/cc-dedicated-toolchain
/opt/cc-dedicated-toolchain/bin/python -m pip install --disable-pip-version-check 'meson==1.9.2'
