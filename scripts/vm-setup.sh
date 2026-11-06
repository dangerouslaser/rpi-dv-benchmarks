#!/bin/bash
# Create (or re-provision) the test VM from vm/rpi5dv.yaml and check it works.
#   scripts/vm-setup.sh              create + start + verify (idempotent)
#   scripts/vm-setup.sh --recreate   delete and create again from the yaml
# Environment: VM_NAME (rpi5dv), VM_CPUS (8), VM_MEMORY (10GiB).
# macOS on Apple Silicon with Lima (brew install lima). On arm64 Linux with
# /dev/udmabuf no VM is needed: scripts/vm.sh runs commands natively.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VM=${VM_NAME:-rpi5dv}
if [ "$(uname -s)" = Linux ]; then
  echo "Linux host: no VM; scripts/vm.sh runs natively (needs aarch64 and /dev/udmabuf)."
  exit 0
fi
command -v limactl >/dev/null || { echo "Lima missing: brew install lima"; exit 1; }
[ "$(uname -m)" = arm64 ] || { echo "Needs Apple Silicon (the VM is aarch64 on Apple's hypervisor)"; exit 1; }
if [ "${1:-}" = --recreate ] && limactl list -q 2>/dev/null | grep -qx "$VM"; then
  limactl delete --force "$VM"
fi
if ! limactl list -q 2>/dev/null | grep -qx "$VM"; then
  cfg=$(mktemp -t rpi5dv).yaml
  sed -e "s|@REPO@|$ROOT|g" -e "s|@CPUS@|${VM_CPUS:-8}|" -e "s|@MEMORY@|${VM_MEMORY:-10GiB}|" \
    "$ROOT/vm/rpi5dv.yaml" >"$cfg"
  limactl create --tty=false --name="$VM" "$cfg"
  rm -f "$cfg"
fi
limactl start --tty=false "$VM"
# Verify: udmabuf, EGL/GLES and Vulkan dmabuf import (needs the build).
if [ -x "$ROOT/build/prefix/bin/probe-dmabuf" ]; then
  "$ROOT/scripts/vm.sh" "$ROOT/build/prefix/bin/probe-dmabuf"
else
  echo "(run scripts/build-all.sh, then scripts/vm.sh build/prefix/bin/probe-dmabuf to verify dmabuf import)"
fi
echo "vm $VM ready: scripts/vm.sh CMD..."
