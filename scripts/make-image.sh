#!/bin/bash
# out/dvbench-rpi5-sd.img.xz: an SD card image for the Raspberry Pi 5 with the
# benchmark ready to run. Raspberry Pi OS Lite (64-bit, trixie), brought up to
# date in an arm64 chroot, plus Mesa's Vulkan/GL tools and v4l-utils; the
# bundle (out/dvbench-rpi5-*.tar.gz, scripts/make-bundle.sh) in /opt/dvbench
# with clips/ (if any); first-boot user dv / dvbench (cloud-init, passwordless
# sudo), hostname dvbench, SSH on, cma-512 for the HEVC decoder.
#   scripts/make-image.sh            (DV_USER / DV_PASSWORD override the login;
#                                      NO_CLIPS=1 leaves the clips out)
# Runs the image work in the test VM (scripts/vm-setup.sh): loop devices and a
# native arm64 chroot. Flash with Raspberry Pi Imager ("Use custom") or
# dd/balenaEtcher to a card of 8 GB or more.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VM=${VM_NAME:-rpi5dv}
BUNDLE=$(ls -t "$ROOT"/out/dvbench-rpi5-*.tar.gz 2>/dev/null | head -1)
[ -n "$BUNDLE" ] || { echo "no bundle: run scripts/build-all.sh first"; exit 1; }
CLIPS=-
[ "${NO_CLIPS:-}" = 1 ] || { ls "$ROOT"/clips/* >/dev/null 2>&1 && CLIPS=$ROOT/clips; }
BASE_URL=${BASE_URL:-https://downloads.raspberrypi.com/raspios_lite_arm64_latest}

run() { limactl shell "$VM" -- sudo env DV_USER="${DV_USER:-dv}" DV_PASSWORD="${DV_PASSWORD:-dvbench}" bash -euo pipefail -c "$1"; }

echo "base image: resolving $BASE_URL"
URL=$(limactl shell "$VM" -- curl -fsIL "$BASE_URL" | tr -d '\r' | awk 'tolower($1)=="location:" {u=$2} END {print u}')
URL=${URL:-$BASE_URL}
echo "base image: $URL"

run "
W=/var/tmp/dvbench-image; mkdir -p \$W; cd \$W
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq parted e2fsprogs dosfstools xz-utils curl >/dev/null
[ -f base.img.xz ] && [ \"\$(cat base.url 2>/dev/null)\" = '$URL' ] || { curl -fsSL -o base.img.xz '$URL'; echo '$URL' > base.url; }
curl -fsSL -o base.sha256 '$URL.sha256'
echo \"\$(cut -d' ' -f1 base.sha256)  base.img.xz\" | sha256sum -c - >/dev/null
echo 'base image: checksum ok'
xz -dc base.img.xz > work.img
truncate -s 7G work.img
parted -s work.img resizepart 2 100%
L=\$(losetup -fP --show work.img)
trap 'R=/mnt/dvbench-image; for d in dev/pts dev sys proc boot/firmware \"\"; do umount \$R/\$d 2>/dev/null || true; done; losetup -d '\$L' 2>/dev/null || true' EXIT
e2fsck -fy \${L}p2 >/dev/null 2>&1 || true
resize2fs \${L}p2 >/dev/null 2>&1
R=/mnt/dvbench-image; mkdir -p \$R
mount \${L}p2 \$R; mount \${L}p1 \$R/boot/firmware
for d in proc sys dev dev/pts; do mount --bind /\$d \$R/\$d; done
mv \$R/etc/resolv.conf \$R/etc/resolv.conf.orig; cp /etc/resolv.conf \$R/etc/resolv.conf
printf '#!/bin/sh\nexit 101\n' > \$R/usr/sbin/policy-rc.d; chmod +x \$R/usr/sbin/policy-rc.d
echo 'chroot: upgrading and installing (several minutes)'
chroot \$R bash -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq &&
  apt-get -y -qq -o Dpkg::Options::=--force-confold full-upgrade >/dev/null &&
  apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers libvulkan1 vulkan-tools mesa-utils-bin \
    libegl1 libgles2 libegl-mesa0 v4l-utils python3 htop >/dev/null && apt-get clean'
chroot \$R dpkg-query -W mesa-vulkan-drivers linux-image-rpi-2712 | sed 's/^/installed: /'
bash '$ROOT/image/customize.sh' \$R '$BUNDLE' '$CLIPS'
rm -f \$R/usr/sbin/policy-rc.d; rm -f \$R/etc/resolv.conf; mv \$R/etc/resolv.conf.orig \$R/etc/resolv.conf
rm -rf \$R/var/lib/apt/lists/* \$R/tmp/*
for d in dev/pts dev sys proc boot/firmware; do umount \$R/\$d; done; umount \$R
e2fsck -fy \${L}p2 >/dev/null; fsck.vfat -a \${L}p1 >/dev/null || true
losetup -d \$L; trap - EXIT
echo 'compressing'
xz -T0 -6 -c work.img > '$ROOT/out/dvbench-rpi5-sd.img.xz'
cd '$ROOT/out' && sha256sum dvbench-rpi5-sd.img.xz > dvbench-rpi5-sd.img.xz.sha256
rm -f \$W/work.img
"
ls -la "$ROOT"/out/dvbench-rpi5-sd.img.xz*
