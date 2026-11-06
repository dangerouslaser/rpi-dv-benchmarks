<img src="assets/icon.svg" width="64" height="64" alt="">

# librelec-rpi5-dv

Dolby Vision on Raspberry Pi 5: benchmark yblod's Vulkan engine against
CroqueMr's CB1 (GLES) on Pi 5 hardware. See [docs/PLAN.md](docs/PLAN.md).

```sh
./scripts/build-all.sh        # aarch64 build in Docker: FFmpeg, libplacebo, CB1, Vulkan engine
```

- `sources.lock` pins every input; `scripts/fetch-sources.sh` fetches into `third_party/`.
- Engine changes live on branch `rpi5` in `third_party/yblod`; `scripts/export-patches.sh`
  records them as `patches/yblod/*.patch`, which a fresh fetch re-applies.
