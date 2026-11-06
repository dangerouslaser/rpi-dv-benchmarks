# Setting up a development machine

Everything is built for aarch64 (the Pi 5's architecture) inside a Docker
container, and run without a Pi in a Debian VM on Mesa's software GPUs.
Only the Pi itself gives performance numbers; the VM is for building,
correctness checks and A/B tests.

## Supported hosts

| Host | Build (Docker) | Run (`scripts/vm.sh`) |
| --- | --- | --- |
| Mac, Apple Silicon | native arm64 containers | Lima VM, `vm/rpi5dv.yaml` |
| Linux aarch64 (e.g. an Ampere box, or the Pi itself) | natively: `scripts/install-native-deps.sh` once, then `build-all.sh` (no Docker) | natively, if `/dev/udmabuf` exists (`sudo modprobe udmabuf`) |
| x86-64 | works under QEMU emulation (`docker run --privileged --rm tonistiigi/binfmt --install arm64`), slowly | not supported (no fast aarch64 VM) |

## macOS (Apple Silicon)

```sh
brew install lima
# Docker: Docker Desktop or OrbStack, either works (scripts use the current context)
brew install ffmpeg dovi_tool mkvtoolnix     # optional: tests/make-test-clips.sh

git clone <this repo> && cd librelec-rpi5-dv
./scripts/build-all.sh      # fetch pinned sources, build everything, make the Pi bundle (~15 min first time)
./scripts/vm-setup.sh       # create the VM from vm/rpi5dv.yaml, provision it, verify dmabuf import
scripts/vm.sh tests/check.sh    # all byte-identical checks (needs clips/, below)
scripts/vm.sh tests/check.sh --validate   # plus the Pi configuration under the Vulkan validation layer
scripts/in-docker.sh bash scripts/build-mesa-shim.sh mesa      # once: Mesa's V3D compiler (as on the Pi)
DVVK_PREPARE_VARIANTS=1 scripts/shader-stats.sh mesa           # V3D threads/registers/spills per pipeline, no Pi
```

`scripts/vm-setup.sh` is idempotent. `--recreate` deletes and rebuilds the VM
from the yaml; `VM_CPUS` / `VM_MEMORY` / `VM_NAME` override 8 CPUs, 10 GiB, `rpi5dv`.
The repo is mounted read-write at the same path inside the VM, so
`scripts/vm.sh` runs any command against the build in place:

```sh
scripts/vm.sh dvbench --hash --layout sand clips/X.mkv
DVVK_NO_INT64=1 DVVK_NO_PUSH_DESCRIPTOR=1 scripts/vm.sh dvbench --engines vk clips/X.mkv   # the Pi 5 engine paths
scripts/vm.sh bash          # a shell, PATH and LD_LIBRARY_PATH set
limactl stop rpi5dv         # when done; limactl delete rpi5dv to remove it
```

Why a VM and not Docker for running: the bench needs real dmabufs
(`/dev/udmabuf`), and neither Docker Desktop's nor OrbStack's kernel has
udmabuf. Debian's cloud kernel does.

## What the VM is

`vm/rpi5dv.yaml`: Debian 13 trixie arm64 (the base of Raspberry Pi OS
trixie), Apple's `vz` hypervisor, the cloud image pinned by digest (daily as
a fallback), Mesa's lavapipe (Vulkan 1.4) and llvmpipe (GLES 3.2), and a
udev rule making `/dev/udmabuf` usable by everyone. A startup probe fails the
boot if `/dev/udmabuf` is missing. Verified: Mesa 25.0.7, kernel 6.12.

## Test clips

Media isn't in git (`clips/` is ignored). Put Dolby Vision files there:

- Profile 7 FEL clips (the project used the "DV FEL Tests" set: "All Layers
  Test (Woman at 80s)", "Power Rangers Credits Test", "Test DT DL P7 CMV4.0
  4000nits v3"; ask Bryan for the shared copy).
- `tests/make-test-clips.sh clips/<P7 FEL>.mkv` derives a Profile 8.1 and an
  MMR single-layer clip from one, covering the engine's other routes.
- Wanted, not yet available: yblod's golden clips (`1917-fel.mkv`,
  `gemini-mel.mkv`, `28yl-p81.mkv`; `tools/golden.sha256`) and a Profile 5 clip.

## Working on the engine

`third_party/` is fetched, not committed. Engine changes:

```sh
cd third_party/yblod            # branch rpi5 = pinned commit + patches/yblod/*.patch
# edit yblod-engine/vk/..., then commit on rpi5
cd ../.. && scripts/export-patches.sh      # rewrites patches/yblod/ from the branch
scripts/in-docker.sh bash scripts/build-vk.sh && scripts/vm.sh tests/check.sh
```

A fresh `scripts/fetch-sources.sh` re-creates branch `rpi5` from the patches.
To move a pin, change `sources.lock` and re-export the patches against it.
