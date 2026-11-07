# Setting up a development machine

Everything is built for aarch64 (the Pi 5's architecture) inside a Docker
container. Without a Pi, it runs in a Debian VM on Mesa's software GPUs.
Only the Pi itself gives performance numbers; the VM is for building and
correctness checks.

## Supported hosts

| Host | Build | Run |
| --- | --- | --- |
| Mac, Apple Silicon | Docker (native arm64 containers) | Lima VM, `vm/rpi5dv.yaml`, through `scripts/vm.sh` |
| Linux aarch64 (an Ampere box, or the Pi itself) | Natively: `scripts/install-native-deps.sh` once, then `build-all.sh` (no Docker) | Natively, if `/dev/udmabuf` exists (`sudo modprobe udmabuf`) or a DMA heap |
| x86-64 | Under QEMU emulation (`docker run --privileged --rm tonistiigi/binfmt --install arm64`), slowly | Not supported (no fast aarch64 VM) |

## macOS (Apple Silicon)

```sh
brew install lima
# Docker: Docker Desktop or OrbStack (the scripts use the current context)
brew install ffmpeg dovi_tool mkvtoolnix     # optional: tests/make-test-clips.sh

git clone <this repo> && cd librelec-rpi5-dv
./scripts/build-all.sh          # fetch FFmpeg, build FFmpeg + the engine + tools, make the Pi bundle
./scripts/vm-setup.sh           # create the VM from vm/rpi5dv.yaml, provision it, check dmabuf import
scripts/vm.sh tests/check.sh    # the engine against tests/golden/hashes.txt (needs clips/, below)
scripts/vm.sh tests/check.sh --validate     # plus the Khronos validation layer
```

`scripts/vm-setup.sh` is idempotent:
- `--recreate` deletes the VM and rebuilds it from the yaml.
- `VM_CPUS`, `VM_MEMORY` and `VM_NAME` override 8 CPUs, 10 GiB and `rpi5dv`.

The repo is mounted read-write at the same path inside the VM, so
`scripts/vm.sh` runs any command against the build in place:

```sh
scripts/vm.sh dvbench --hash clips/X.mkv
scripts/vm.sh dvbench --filters yblod --mmr float clips/X.mkv
scripts/vm.sh bash          # a shell, PATH and LD_LIBRARY_PATH set
limactl stop rpi5dv         # when done; limactl delete rpi5dv to remove it
```

Why a VM and not Docker for running: `dvbench` needs real dmabufs
(`/dev/udmabuf`), and neither Docker Desktop's nor OrbStack's kernel has
udmabuf. Debian's cloud kernel does.

**What the VM is** (`vm/rpi5dv.yaml`):
- Debian 13 trixie arm64, the base of Raspberry Pi OS trixie.
- Apple's `vz` hypervisor.
- The cloud image is pinned by digest, with the daily image as a fallback.
- Mesa's lavapipe for Vulkan 1.4.
- A udev rule that makes `/dev/udmabuf` usable by everyone. A startup probe
  fails the boot if it's missing.

## On the Pi

Two ways to get there:
- **Flash an image:** `scripts/make-image.sh` builds `out/dvbench-rpi5-sd.img.xz`.
  It is Raspberry Pi OS Lite with the bundle and `clips/`, first-boot user
  `dv` / `dvbench`, SSH on, and `cma-512` set.
- **Use an existing install:** copy `out/dvbench-rpi5-*.tar.gz` across. See
  `bundle/README.md`.

To build on the Pi itself: `scripts/install-native-deps.sh`, then
`scripts/build-all.sh`.

## Test clips

Media isn't in git (`clips/` is ignored). `tests/check.sh` runs the cases in
`tests/golden/hashes.txt` whose clips are present:

- **The DV FEL test set** (Profile 7 FEL): "DV FEL All Layers Test (Woman at
  80s)", "DV FEL Power Rangers Credits Test" and "DV FEL Test DT DL P7 CMV4.0
  4000nits v3". Ask Bryan for the shared copy.
- **Derived clips:** `tests/make-test-clips.sh` makes profile 8.1 (`p81-`), MEL
  (`mmr-`) and profile 8.1 MMR (`p81mmr-`) clips from Profile 7 sources:
  - The test set's derivatives.
  - The `p81-1917` and `p81mmr-1917` cuts: 6–8 s of 1917 from 10:00.
  - The `p81mmr-al`, `-pr`, `-cmv` and `-dune2` cuts: the first 4 s of each
    test clip, and 6 s of Dune Part Two from 50:00.

Full films (1917, Dune) were measured from the NAS over SMB. See
`docs/ENGINE.md`.

## Working on the engine

- **Edit:** `engine/pi_engine.c`, `engine/shaders/*`, and `engine/planner/*`
  (vendored; note changes in its README).
- **Build:** `scripts/in-docker.sh bash scripts/build-engine.sh`.
- **Check:** `scripts/vm.sh tests/check.sh`, then measure on the Pi.

A change that alters output on purpose needs new golden hashes, and a
reason in the commit.

Follow the shader rules in `docs/ENGINE.md` ("What V3D taught us"). After any
shader change, check the V3D compiler's view on the Pi:
`V3D_DEBUG=shaderdb dvbench --frames 1 …` gives threads, loops and spills.
