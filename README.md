<img src="assets/icon.svg" width="64" height="64" alt="">

# librelec-rpi5-dv

Dolby Vision on Raspberry Pi 5: benchmark yblod's Vulkan engine against
CroqueMr's CB1 (GLES) on Pi 5 hardware. Status and findings: [docs/PLAN.md](docs/PLAN.md).

New machine: [docs/SETUP.md](docs/SETUP.md). In short (Apple Silicon Mac, Docker, `brew install lima`):

```sh
./scripts/build-all.sh          # aarch64 in Docker: FFmpeg, libplacebo, CB1, Vulkan engine, dvbench, Pi bundle
./scripts/vm-setup.sh           # Debian arm64 VM from vm/rpi5dv.yaml (udmabuf, lavapipe/llvmpipe)
scripts/vm.sh tests/check.sh    # every byte-identical check on clips/
scripts/vm.sh dvbench --hash clips/X.mkv
```

On the Pi: flash `out/dvbench-rpi5-sd.img.xz` (`scripts/make-image.sh`: Raspberry Pi OS Lite
+ the bundle + clips, user `dv`/`dvbench`, SSH, cma-512), or copy `out/dvbench-rpi5-*.tar.gz`
to an existing install; see [bundle/README.md](bundle/README.md).

- `sources.lock` pins every input; `scripts/fetch-sources.sh` fetches into `third_party/`.
- Engine changes live on branch `rpi5` in `third_party/yblod`; `scripts/export-patches.sh`
  records them as `patches/yblod/*.patch`, which a fresh fetch re-applies.
- `bench/`: `dvbench` (both engines on the same DRM PRIME pictures), `probe-dmabuf`.
- `vm/rpi5dv.yaml`: the test VM; `tests/`: `check.sh` (A/B suite), `make-test-clips.sh`.
- `clips/` (not in git): test media.
