# dvbench for the Raspberry Pi 5

Measures the Pi Dolby Vision engine on the Pi's own hardware decoder. It
reconstructs each picture (BL, the FEL enhancement layer, the RPU) and packs
it, with its metadata, into the TV-led HDMI tunnel frame. There's no Kodi,
no display and no audio.

## Setup (Raspberry Pi OS trixie, 64-bit)

The decoder needs more contiguous memory than the default (64 MB on some
boards):
1. In `/boot/firmware/config.txt`, replace the `dtoverlay=vc4-kms-v3d` line
   with `dtoverlay=vc4-kms-v3d,cma-512`.
2. Reboot.

`run-bench.sh` warns if CMA is under 256 MB. The SD image
(`scripts/make-image.sh`) is already set up this way.

```sh
sudo apt install mesa-vulkan-drivers libvulkan1 vulkan-tools v4l-utils python3
tar xzf dvbench-rpi5-*.tar.gz && cd dvbench-rpi5-*
sudo usermod -aG video,render $USER   # then log in again
```

## Run

```sh
./run-bench.sh /path/to/clips/*.mkv              # everything (a few minutes a clip)
./run-bench.sh --quick /path/to/clips/*.mkv      # the correctness checks only
./run-bench.sh --start 600 --play 300 film.mkv   # 5 minutes of real time, 10 minutes in
./run-bench.sh --filters phase p81mmr-*.mkv      # force one filter set for every clip
```

Each clip runs with CB1's filters unless the engine refuses its plan under
CB1's contract (profile 8.1 with MMR chroma, e.g. `p81mmr-*`); that clip then
runs with yblod's, and the summary says which. `--filters` forces one set.
Other options: `--frames`, `--throughput`, `--repeat`, `--out`. Run it with
no clips for the list. It exits 1 if any run failed or any check differed.

Per clip, all from the Pi's hardware decoder (V4L2 request, SAND128):

1. **Correctness.** The output from hardware decoding must equal the output
   from software decoding of the same pictures, byte for byte. Every exact
   MMR mode must equal the others.
2. **Speed.** Three timed runs. Each reports:
   - decode fps
   - per picture: plan + render + GPU finish, median and p95
   - throughput: three frames in flight, as in playback

   Clocks, temperature and throttling are recorded around every run.
   Throttled runs are left out of the medians.
3. **MMR modes and passes.** Times for the exact (default), split and float
   modes, with per-pass GPU times.
4. **Real-time playback** for `--play` seconds (default 60). Decoding runs on
   its own thread and rendering follows the stream's frame clock. It counts
   pictures that missed their display deadline, and decode stalls.
5. **V3D compiler statistics** for the engine's shaders: threads,
   instructions, spills.

Everything lands in `results/HOST-DATE/`: `summary.txt`, `system.txt`, and a
log and JSON per run. Send that directory back.

- Let `run-bench.sh` set the CPU governor. It needs passwordless `sudo`, the
  default on Raspberry Pi OS.
- Without a cooler, watch the throttling column. Don't overclock the GPU for
  these figures.

## Running dvbench directly

```sh
export LD_LIBRARY_PATH=$PWD/lib
bin/dvbench --source v4l2 --frames 12 --warmup 2 --throughput 10 clip.mkv
bin/dvbench --source v4l2 --start 600 --frames 4 --play 60 film.mkv
bin/dvbench --help    # the option list
```

- `--filters phase` (or `yblod`) selects YBLOD's spatial filters. Use it for
  profile 8.1 streams with MMR chroma, which CB1's contract (the default)
  doesn't take.
- `--mmr exact|split|exact64|float` selects the MMR mode.
- `--probe` checks the dmabuf allocator, the Vulkan device and an output
  import, and nothing else.
- Without `--source v4l2`, dvbench decodes in software (`--source sw`).
- `PI_PROFILE=1` adds per-pass GPU times.

