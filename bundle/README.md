# dvbench for the Raspberry Pi 5

Benchmarks yblod's Dolby Vision Vulkan engine against CroqueMr's CB1 (GLES)
on the same decoded pictures. No Kodi, no display, no audio.

## Setup (Raspberry Pi OS trixie, 64-bit)

```sh
sudo apt install mesa-vulkan-drivers libvulkan1 libegl1 libgles2 vulkan-tools mesa-utils-bin v4l-utils
tar xzf dvbench-rpi5-*.tar.gz && cd dvbench-rpi5-*
sudo usermod -aG video,render $USER   # then log in again
```

## Run

```sh
./run-bench.sh /path/to/clips/*.mkv
```

Per clip, with the Pi's hardware decoder (V4L2 request, SAND128 frames):

- decode speed: pictures decoded per second (BL and, for Profile 7, the EL);
- per picture, each engine on its own: import + render + GPU finish,
  median / p95 milliseconds;
- throughput: each engine flat out for 20 s, pipelined as in playback,
  frames per second and CPU milliseconds per frame;
- a correctness check first: CB1's and the Vulkan engine's 3840x2160 output,
  byte for byte, on 12 pictures, and the Vulkan engine's output from the
  hardware decoder vs software decoding (all should be identical);
- each performance run 3 times, engine order alternating, with clocks,
  temperature and throttling recorded (throttled runs are left out of the
  medians in `table.txt`);
- real-time playback for 30 s per engine: decoding on its own thread,
  rendering at the stream's frame rate; late pictures and decode stalls;
- the Vulkan engine's per-stage GPU times;
- the same with software decoding, for reference.

Use an active cooler, and let `run-bench.sh` set the CPU governor
(it needs passwordless `sudo`, the default on Raspberry Pi OS). `--quick`
runs only the correctness checks (a couple of minutes).

Everything lands in `results/HOST-DATE/` (`summary.txt`, `table.txt`,
`system.txt`, a log and a JSON per run). Send that directory back.

Useful clips: 4K Profile 7 FEL (two decodes per picture: the heaviest),
Profile 7 MEL, Profile 8.1, Profile 5.

## Notes

- Both engines receive the decoder's SAND128 buffers. The Vulkan engine
  unpacks them in a compute pass; CB1 (which reads linear P010 planes) gets
  the same unpacking as a GLES pass in the bench, counted in its time.
- `dvbench --help`-style options are at the top of `bin/dvbench`'s source
  (`bench/dvbench.c`); `run-bench.sh` is the intended entry point.
- If the EL fails to decode in hardware next to the BL, run-bench retries
  with the EL in software (`--el-sw`).
