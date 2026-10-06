#!/bin/bash
# One-time root setup of WSL Ubuntu 22.04 for the Steam Frame cross build.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

dpkg --add-architecture arm64
# Existing sources serve amd64 only; arm64 comes from ports.
sed -i -E 's/^deb (http)/deb [arch=amd64] \1/' /etc/apt/sources.list
cat > /etc/apt/sources.list.d/arm64-ports.list <<EOF
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
EOF

apt-get -qq update
apt-get -y install build-essential cmake git pkg-config rsync sshpass \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  libcurl4-openssl-dev:arm64 mesa-common-dev:arm64 libxxf86dga-dev:arm64 \
  libxrandr-dev:arm64 libxxf86vm-dev:arm64 libasound2-dev:arm64 libsdl2-dev:arm64 \
  libx11-xcb-dev:arm64 libxcb-glx0-dev:arm64 libxcb1-dev:arm64 libgl-dev:arm64
# Windows PATH entries (MSYS2) leak into CMake package searches.
grep -q appendWindowsPath /etc/wsl.conf || printf '\n[interop]\nappendWindowsPath=false\n' >> /etc/wsl.conf
aarch64-linux-gnu-gcc --version | head -1
