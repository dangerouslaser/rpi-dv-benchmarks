#!/bin/bash
# Debian 13 arm64 VM (Lima, Apple vz) for tests that need real dmabufs:
# its kernel has udmabuf, Docker Desktop's and OrbStack's don't. Runs the
# aarch64 build from build/prefix on Mesa lavapipe/llvmpipe.
#   scripts/vm-setup.sh            create + provision (idempotent)
#   scripts/vm.sh CMD...           run CMD in the VM at this repo
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VM=rpi5dv
if ! limactl list -q 2>/dev/null | grep -qx $VM; then
  limactl create --name=$VM --tty=false --vm-type=vz --cpus=8 --memory=10 --disk=40 \
    --mount-writable --mount="$ROOT" template:debian-13
fi
limactl start $VM >/dev/null 2>&1 || true
limactl shell $VM -- sudo bash -euo pipefail -c '
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    mesa-vulkan-drivers vulkan-tools libvulkan1 libegl-mesa0 libegl1 libgles2 libgbm1 \
    libdrm2 libva2 libva-drm2 libudev1 python3 gcc libc6-dev >/dev/null
  echo "KERNEL==\"udmabuf\", MODE=\"0666\"" >/etc/udev/rules.d/99-udmabuf.rules
  udevadm control --reload; udevadm trigger --name-match=udmabuf; chmod 666 /dev/udmabuf'
echo "vm $VM ready"
