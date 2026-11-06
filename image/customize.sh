#!/bin/bash
# Customize a mounted Raspberry Pi OS (trixie, arm64) root at $1 (boot
# partition at $1/boot/firmware) into the dvbench test image. Run as root on
# an arm64 Linux host (scripts/make-image.sh does it in the test VM).
#   customize.sh ROOT BUNDLE.tar.gz CLIPS_DIR|-
set -euo pipefail
R=$1 BUNDLE=$2 CLIPS=${3:--}
[ -f "$R/etc/os-release" ] && [ -d "$R/boot/firmware" ] || { echo "not a mounted RPi OS root: $R"; exit 1; }

# The benchmark, and its clips
rm -rf "$R/opt/dvbench" && mkdir -p "$R/opt/dvbench"
tar -C "$R/opt/dvbench" --strip-components=1 -xzf "$BUNDLE"
if [ "$CLIPS" != - ]; then
  mkdir -p "$R/opt/dvbench/clips"
  cp "$CLIPS"/* "$R/opt/dvbench/clips/"
fi
chmod -R a+rX "$R/opt/dvbench"

# Every new user (the first-boot one included): shortcuts and a note
mkdir -p "$R/etc/skel"
ln -sfn /opt/dvbench "$R/etc/skel/dvbench"
ln -sfn /opt/dvbench/clips "$R/etc/skel/clips"
cat >"$R/etc/skel/README-dvbench.txt" <<'EOF'
Dolby Vision benchmark (yblod Vulkan engine vs CB1 GLES) on the Raspberry Pi 5.

  cd ~ && dvbench/run-bench.sh --quick clips/*      # correctness only, a few minutes
  cd ~ && dvbench/run-bench.sh clips/*              # everything (long: 15-30 min per clip)

Results land in ~/results/HOST-DATE/. Copy that directory back, e.g. from
your computer:  scp -r dv@dvbench.local:results .   (or the Pi's IP address)

dvbench/README.md has the details. CMA is already set (cma-512 in
/boot/firmware/config.txt). Use an active cooler.
EOF

# First-boot user, hostname, SSH: cloud-init user-data, as Raspberry Pi Imager writes it
HASH=$(openssl passwd -6 "${DV_PASSWORD:-dvbench}")
cat >"$R/boot/firmware/user-data" <<EOF
#cloud-config
hostname: dvbench
manage_etc_hosts: true
users:
  - name: ${DV_USER:-dv}
    gecos: Dolby Vision bench
    groups: [adm, dialout, cdrom, audio, users, sudo, video, games, plugdev, input, gpio, spi, i2c, netdev, render]
    shell: /bin/bash
    lock_passwd: false
    passwd: "$HASH"
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
ssh_pwauth: true
EOF
touch "$R/boot/firmware/ssh"
chroot "$R" systemctl enable ssh >/dev/null 2>&1 || true

# The HEVC decoder's pictures need CMA (Raspberry Pi OS's default is 64 MB on some boards)
sed -i 's/^dtoverlay=vc4-kms-v3d$/dtoverlay=vc4-kms-v3d,cma-512/' "$R/boot/firmware/config.txt"
grep -q 'vc4-kms-v3d,cma-512' "$R/boot/firmware/config.txt" || { echo "config.txt: no vc4-kms-v3d line to extend"; exit 1; }

echo "customized: $(du -sh "$R/opt/dvbench" | cut -f1) in /opt/dvbench; user ${DV_USER:-dv}; hostname dvbench"
