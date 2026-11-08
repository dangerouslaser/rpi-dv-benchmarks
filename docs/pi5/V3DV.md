# Raspberry Pi 5 GPU and the v3dv Vulkan driver: engineering reference

What the hardware is, and how Mesa's v3dv maps Vulkan onto it. The engine's
design (`../ENGINE.md`) rests on it. Compiled 2026-11-06 from three sources:

- **Device**: the Pi 5 itself, in `device/`:
  - `v3dv-vulkaninfo.txt`: `vulkaninfo` for the V3D GPU alone
  - `vulkaninfo.txt`: the full `vulkaninfo` (instance, layers, V3D and llvmpipe)
  - `v3dv-profile.json`: the V3D device as a Vulkan profile
  - `v3d_ident.txt`: the kernel's `v3d_ident` and the v3d debugfs listing
  - `system.txt`: CPU, memory, kernel, firmware, clocks, config, DRM, V4L2,
    DMA heaps, CMA, packages
- **Source**: Mesa 26.2.2 (commit 3281a69a8b; the Pi runs 26.2.6), Mesa main
  (09e600cd27), and the v3d kernel driver (rpi-6.18.y and a 7.3 tree). None of
  these trees is kept in this repository. Paths below are relative to Mesa's
  `src/broadcom/` unless marked `K/` (the kernel's `drivers/gpu/drm/v3d/`).
- **Published material**: Igalia talks and blogs, Raspberry Pi documentation,
  and Mesa merge requests and commits. URLs are given inline.

Tags: **[S]** verified in source, **[D]** read from the device, **[P]** published,
**[I]** inferred. Broadcom has never published a V3D 4.x/7.x specification
(https://docs.mesa3d.org/drivers/v3d.html). Mesa, the kernel and Igalia's slides
are the only public ground truth.

## 0. Target platform: LibreELEC 13

The target is LibreELEC 13, built from LibreELEC.tv master (checked at commit
a0fa50bc6e, 2026-11-06). For the Pi 5 it ships, against the Raspberry Pi OS
install the device dumps and the bench figures come from:

| Component | LibreELEC 13 | Raspberry Pi OS trixie |
|---|---|---|
| Kernel | raspberrypi/linux 0b41724 = **6.18.60** | 6.18.60 (same) |
| Mesa | **26.2.5** | 26.2.6 |
| FFmpeg | 9.0.2 + `rpi`, `postproc` and `libreelec` patches | the bench's: 9.0 + the same sets (`patches/ffmpeg/`) |
| Kodi | master, GBM + GLES, video by DRM PRIME, 11 RPi patches (e.g. max bpc for high-bit-depth video) | — |
| Boot config | `dtoverlay=vc4-kms-v3d,cma-512` already set | the bench's image sets the same |

What this means:
- **Measurements transfer directly**: same kernel, same Mesa branch. The
  engine's times on LibreELEC match those on Raspberry Pi OS.
- **Vulkan isn't built for RPi by default:** `projects/RPi/options` sets
  `VULKAN="no"`. `config/graphic` maps `vc4` to Mesa's `broadcom` Vulkan
  driver, so `VULKAN="vulkan-loader"` in `projects/RPi/devices/RPi5/options`
  builds v3dv, the loader and `vulkaninfo` (`patches/libreelec/`).
- **Kernel config:**
  - `CONFIG_TRANSPARENT_HUGEPAGE` is off, so v3d "super pages" are off (Igalia
    measured +1–4% with them). This is a one-line config change.
  - `CONFIG_UDMABUF` is off: the bench allocates its outputs from the system
    DMA heap there (CMA is the decoder's).
  - `PERF_EVENTS`, `FTRACE` and `DEBUG_FS` are on, so v3d tracepoints and
    perf counters are usable.
- **The kernel's job model (§2) is a design property, not a version limitation.**
  The cache clean after every compute job and one job at a time per queue are
  the same in rpi 6.18 and mainline 7.3. The engine avoids them rather than
  waiting for a fix.

## 1. The hardware

| | Pi 5 (BCM2712) | |
|---|---|---|
| GPU | V3D 7.1.10.16 ("VideoCore VII") | [D] v3d_ident |
| Layout | 3 slices × 4 QPUs = **12 QPUs**, **3 TMUs** (one per slice), no L3 cache | [D] |
| GPU clock | **1150 MHz** (current firmware `v3d_freq`; docs say 960, spec sheets 800) | [D] [P] |
| SIMD | 16 lanes per QPU thread (`V3D_CHANNELS`), subgroup size 16. Executed as 4 lanes/clock over 4 clocks (VC4/VC6 design; unverified for 7.1) | [S] common/v3d_limits.h:30, [I] |
| ALUs | scalar only (`lower_to_scalar`); one add ALU and one mul ALU, dual-issue; **no FMA**; SFU for rcp/rsqrt/exp2/log2/sin | [S] [P] |
| Registers | 64 physical per QPU; **64 per thread at 1–2 threads, 32 at 4 threads**; no accumulators on 7.1 | [S] compiler/vir_register_allocate.c:35,1144-1158,1523-1525 |
| Peak (derived) | 12 QPU × 4 lanes × 2 ops × 1.15 GHz ≈ **110 GFLOP/s**, ≈ 55 G lane-instructions/s → ~**275 instruction slots per pixel at 4K24**, ~110 at 4K60, for the whole pipeline | [I] |
| Tile buffer (TLB) | 16 KB colour + 16 KB depth + 8 KB aux depth; up to 8 render targets | [S] common/v3d_util.c:101-200 |
| Memory | 32-bit LPDDR4X-4267, ~17 GB/s theoretical, ~11 GB/s measured (CPU STREAM) | [P] raspberrypi.com, weak 3rd-party |
| CPU | 4× Cortex-A76 @ 2.4 GHz | [D] |
| HEVC decoder | separate block, 910 MHz; V4L2 stateless; outputs **NV12/P030 in SAND128 columns** | [D] [P] |
| Display (HVS) | scans out SAND128 NV12/P030, linear P010, 10-bit RGB directly; does YCbCr→RGB (BT.2020) and scaling; **cannot scan out UIF** (the GPU's tiled layout) | [P] K vc4_plane.c |
| HDMI | up to 12 bpc; RGB/YUV444/YUV422 output (no 4:2:0 output) | [P] vc4_hdmi.c |

The TFU (texture formatting unit, a fixed-function copy/detile engine) is a
contradiction. `v3d_ident` says "TFU: no", but the kernel always reports TFU
support and v3dv submits TFU jobs for copies. The ident bit is probably decoded
wrongly for 7.x; a measurement can settle it.

## 2. How v3dv runs work: jobs

This is the most important section. On a desktop GPU, a dispatch is a few
commands in a ring. On v3dv, **every dispatch is a kernel job**.

### Compute (`vkCmdDispatch`)

- Each dispatch becomes its own `V3DV_JOB_TYPE_GPU_CSD` job, and each job its
  own `DRM_IOCTL_V3D_SUBMIT_CSD` ioctl. v3dv never merges dispatches.
  [S] vulkan/v3dv_cmd_buffer.c:4242-4383, vulkan/v3dv_queue.c:899-967
- The kernel pairs every CSD job with a **cache-clean job**. [S] K/v3d_submit.c:529-557
- **Before** each dispatch: invalidate L2C, flush L2T, invalidate the slice caches. [S] K/v3d_sched.c:353, K/v3d_gem.c:263-275
- **After** each dispatch: flush the TMU write-combiner, clean L2T. The CPU busy-waits on both. [S] K/v3d_gem.c:215-245
- Each kernel queue runs **one job at a time** (`credit_limit = 1`). The hardware's
  "overlap with previous dispatch" bit is never set. [S] K/v3d_sched.c:825-830, common/v3d_csd.h:30
- Every job attaches its fence to **all** its BOs as a *write*, so consecutive
  dispatches that share any BO (shader code, uniforms, descriptors) serialise even
  with no `vkCmdPipelineBarrier`. The source carries an
  `XXX: Use shared fences for read-only objects`. [S] K/v3d_submit.c:418-433
- **Net effect [I]:** a chain of N dispatches does N rounds of: drain the GPU, take
  the IRQ, run the scheduler, clean the caches, invalidate the caches, start cold.
  The cache is cold at the start of every dispatch.
- `vkCmdDispatchIndirect` is a CPU job: the kernel reads the dispatch size on the
  CPU, then launches. [S] vulkan/v3dv_cmd_buffer.c:4404-4450
- Workgroups:
  - At most 256 invocations. Larger workgroups are silently rewritten as a loop.
  - 16 invocations make one "batch" (one QPU thread).
  - Supergroups pack up to 16 workgroups when the size isn't a multiple of 16.
    Subgroup operations disable that packing, and barriers cap it.
  - **Use multiples of 16.**

  [S] common/v3d_util.c:32-99, vulkan/v3dv_pipeline.c:3150-3165, compiler/vir.c:1810-1837
- **Shared memory is DRAM.** It is a BO of `shared_size × workgroups`, accessed
  through the TMU exactly like an SSBO. [S] vulkan/v3dv_cmd_buffer.c:4332-4340, compiler/nir_to_vir.c:584-589,4905-4928

### Render passes (fragment shaders)

- A render pass becomes **one** CL job (a binning list plus a render list,
  submitted with one ioctl). Many draws, and subpasses with identical attachments,
  share that job. [S] vulkan/v3dv_cmd_buffer.c:323-388, vulkan/v3dv_queue.c:728-860
- The kernel adds a cache-clean job only if a shader in the pass wrote memory
  through the TMU (SSBO or image stores). Plain fragment output needs none.
  [S] vulkan/v3dv_queue.c:762-763, K/v3d_submit.c:1209-1219
- Fragment output goes to the **on-chip TLB**. It is stored to memory once per
  tile, in the image's own layout (raster/linear or UIF). [S] vulkan/v3dvx_cmd_buffer.c:342-400
- Tile size depends on the colour bytes per pixel. Depth is always counted at
  4 B/px (FIXME). [S] common/v3d_util.c:121-177, vulkan/v3dv_pass.c:335-369

  | Colour bytes per pixel, all render targets | Tile |
  |---|---|
  | 4 (one RGBA8 / R32UI / RGB10A2) | 64×64 |
  | 8–16 (RGBA16UI, 2–4 × 32-bit, RGBA32UI) | 64×32 |
  | 32 | 32×32 |
  | 64 | 32×16 |
- Integer, 32-bit float and 16-bit unorm/snorm targets use the TLB's
  32-bit-per-channel path; everything else uses F16. [S] compiler/nir_to_vir.c:1953-1985
- `loadOp=LOAD` costs a per-tile read; `CLEAR` and `DONT_CARE` don't. A render
  area that isn't tile-aligned turns clears into draws.
- Double-buffered tiles (which hide store latency) are off by default (`V3D_DEBUG=db`). [S] vulkan/v3dv_cmd_buffer.c:1620-1632
- **Things that split a render job** [S] vulkan/v3dv_cmd_buffer.c:3331-3466,4196-4201:
  - a barrier inside a render pass
  - a timestamp inside a render pass
- **Mesa main** reads input attachments straight from the tile buffer
  (`nir_load_tile_image`). 26.2 reads them from memory through the TMU. That
  lets chained subpasses stay on-chip. [S] mesa-main vulkan/v3dv_pipeline.c:300-360

### Submission and synchronisation

- One Vulkan queue (graphics + compute + transfer). The kernel has separate
  BIN, RENDER, TFU, CSD, CACHE_CLEAN and CPU queues, each one job at a time.
  `vkQueueSubmit` issues one ioctl per job. [S] K/v3d_sched.c:825-876, vulkan/v3dv_queue.c:1076-1131
- **Timestamps are CPU jobs**: `ktime_get_ns()` at kernel scheduling time,
  serialised against all prior work. They are not GPU clocks.
  [S] vulkan/v3dv_queue.c:600-605, K/v3d_sched.c:437-450
- **VkEvents are implemented with compute shaders**: a spin-wait and a dispatch.
  Avoid them. [S] vulkan/v3dv_event.c
- After 50 ms idle the GPU runtime-suspends (clock gated).
  [P] https://mairacanal.github.io/bringing-runtime-pm-to-raspberry-pi-gpu/

## 3. Memory access from shaders

Each kind of access takes a different hardware path:

| Access | Hardware path | Notes |
|---|---|---|
| SSBO / shared / global load-store, atomics | TMU "general" lookups | Up to 4 × 32-bit per lookup, never crossing 16 bytes; 8/16-bit always scalar. [S] compiler/nir_to_vir.c:2182-2213 |
| UBO, push constants, **readonly** SSBO at a **uniform** offset | `ldunifa` stream, not the TMU | Only for non-divergent offsets. Supergroups make the workgroup ID divergent. [S] compiler/nir_to_vir.c:3241-3300 |
| Sampled image, texel buffer | TMU texture path, L2T cache | 16-bit formats return packed halves (2 words for 4 channels). [S] compiler/v3d_tex.c:240-252 |
| Storage image | TMU write/read; the shader packs and unpacks the format | [S] compiler/v3d_nir_lower_image_load_store.c |
| Fragment output | **TLB (on-chip)** | Stored once per tile |

- **Latency hiding:**
  - Up to 8 TMU operations are in flight per thread.
  - The result FIFO holds `16 / threads` words per thread, so 4 at 4 threads.
  - Results are collected with a thread switch and `ldtmu`.
  - A single access needing more FIFO than that forces fewer threads.

  [S] compiler/v3d_compiler.h:45-62, compiler/nir_to_vir.c:252-293,696-701
- **Image layouts:**
  - Optimal tiling means UIF (or UBLINEAR/LT for small levels). Linear tiling means raster. [S] vulkan/v3dv_image.c:186-226
  - **Linear 2D images can't be sampled or used as storage images.** Sampling one
    silently adds a copy into a tiled shadow image ("…requires a copy").
    [S] vulkan/v3dv_formats.c:150-191, vulkan/v3dv_cmd_buffer.c:3575-3700
- **DRM format modifiers:** v3dv supports **only LINEAR and BROADCOM_UIF, no SAND**,
  in 26.2 and in main. [S] vulkan/v3dv_formats.c:362-405. The GL driver imports
  SAND128/P030 and detiles them into a UIF shadow texture: a fragment blit in
  26.2, the TFU in main since Oct 2026. [S] gallium v3d_blit.c; [P] mesa e2f05fca
- **YCbCr sampler conversion:** only 8-bit 4:2:0 (NV12 and 3-plane), OPTIMAL
  tiling only, no filtering. **No P010.** [S] vulkan/v3dvx_formats.c:292-302
- **Host memory:** one memory type, DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT,
  **not cached**. [D] v3dv-vulkaninfo.txt:493-499

## 4. The shader compiler: what costs what

- **Thread ladder** (first strategy that compiles wins):
  1. 4 threads (32 registers, no spills allowed)
  2. 4 threads, fewer optimisations
  3. 2 threads with spilling
  4. 2 threads, fewer optimisations
  5. 2 threads, no TMU pipelining
  6. 2 threads, fallback scheduler

  Spills go to DRAM through the TMU. Fragment and compute shaders follow the
  same ladder. [S] compiler/vir.c:2192-2199,2610-2650

| Operation | Cost | Source |
|---|---|---|
| float add/mul/min/max | 1 slot on its ALU; add and mul can dual-issue; no FMA | [S] |
| `rcp`/`rsqrt`/`exp2`/`log2`/sin/cos | SFU, ~3 instructions of latency | [S] compiler/nir_to_vir.c:1742-1772 |
| 32-bit `imul` | 2 instructions (MULTOP + UMUL24). `umul24` is native but **unreachable from SPIR-V** | [S] compiler/v3d_compiler.h:1584-1588 |
| `mul_high` / `umulExtended` | multi-instruction sequence | [S] vulkan/v3dv_pipeline.c:201 |
| integer divide / modulo | multiply-shift for constant divisors; otherwise float-reciprocal sequence | [S] compiler/vir.c:1999-2003 |
| `bitfieldExtract/Insert`, `bitCount`, `findLSB` | lowered to sequences (`findMSB` uses CLZ) | [S] vulkan/v3dv_pipeline.c:172-206 |
| int64 / float64 | **not supported** (`shaderInt64 = false`) | [S] vulkan/v3dv_device.c:369-370 |
| int8 / int16 arithmetic | widened to 32-bit: no speed-up | [S] compiler/vir.c:719-797 |
| fp16 | native for add/sub/mul/min/max/neg/abs only, scalar (no 2×16 packing) | [S] compiler/vir.c:744-753 |
| 4×8-bit dot product | native on 7.1 (`udot`/`sdot`) | [S] [P] mesa 3a8d5aea |
| vectors (`uvec4` maths) | 4× the scalar cost: all ALUs are scalar | [S] |
| divergent branches | both sides execute under a lane mask | [S] compiler/nir_to_vir.c:4293-4480 |
| loops | unrolled up to 16 iterations | [S] vulkan/v3dv_pipeline.c:215 |

## 5. Features (Mesa 26.2.6 on V3D 7.1)

- **Present** [D] [S] vulkan/v3dv_device.c:
  - Vulkan 1.3, conformant since Aug 2024
  - subgroups (size 16, compute and fragment): basic, vote, ballot, shuffle,
    relative, arithmetic, clustered, quad, rotate
  - `shaderInt16`/`Int8`/`Float16` (lowered, per §4)
  - 8- and 16-bit storage, scalar block layout
  - timeline semaphores, dynamic rendering, buffer device address (32-bit),
    inline uniform blocks
  - dma-buf import and `image_drm_format_modifier` (LINEAR and UIF only)
  - `KHR_performance_query`, `KHR_pipeline_executable_properties`
- **Missing**:
  - `VK_KHR_push_descriptor` (also missing in main)
  - descriptor indexing and descriptor buffers
  - int64 / float64 and 64-bit atomics
  - 10/16-bit YCbCr formats, SAND modifiers
  - calibrated timestamps, external host memory
  - a separate compute or transfer queue
- **Mesa main adds**:
  - `KHR_maintenance6/7/8`
  - `EXT_host_image_copy`
  - rasterization-order attachment access
  - tile-buffer input attachments
  - Perfetto GPU tracepoints (Sep 2026; probably 26.3+)

  The compute and submit paths are effectively unchanged.

## 6. Tools for measuring

- **`VK_KHR_performance_query`**: V3D 7.1's counters (180 in the kernel), including
  per-cause QPU stalls (TMU/TLB/SFU/uniforms), wait bubbles, TMU active/stalled,
  L2T hits/misses, AXI bytes read/written, compute-active cycles and total cycles.
  Counters are grouped into kernel perfmons, and a perfmon serialises all queues.
  [S] common/v3d_performance_counters.h:37-133, K/v3d_perfmon.c
- **`VK_KHR_pipeline_executable_properties`**: per-pipeline instructions, threads,
  spills/fills, read stalls and compile strategy.
- **`V3D_DEBUG`**:
  - `perf`: fallback and slow-path warnings
  - `shaderdb`: shader statistics
  - `cs`/`fs`/`qpu`/`vir`/`nir`: shader dumps
  - `cl`/`clif`: command-list dumps
  - `sync`: wait after every job (isolates per-job time)
  - `db`: double-buffered tiles
  - `tmu16`/`tmu32`, `noloopunroll`, `no_merge_jobs`, `disable_tfu`

  [S] common/v3d_debug.c:43-108
- **Kernel tracepoints** `v3d_submit_csd`, `v3d_csd_irq`, `v3d_cache_clean_begin/end`
  separate job overhead from shader time. [S] K/v3d_trace.h

## 7. The video path

- **Decoder → display (stock Kodi on LibreELEC):** DRM PRIME "direct to plane".
  The HVS scans the decoder's SAND buffers out directly, with zero GPU work. HDR
  works only on this path.
- **Decoder → engine → display (`patches/kodi/`):** Kodi's DRM PRIME renderer
  hands each Dolby Vision picture's SAND buffers to the engine, which renders
  the tunnel frame into a linear XRGB8888 buffer; that goes on the same plane
  1:1, behind the render's sync_file.
  [P] https://forum.libreelec.tv/thread/28629-raspberry-pi-5-hdr-not-working
- **Decoder → GPU:**
  - **GL**: imports SAND and detiles it to UIF.
    Igalia measured 4K 10-bit HEVC import plus a trivial render on Pi 5 at
    **~18 ms of GPU per frame, 57.7 fps, before any DV processing** (5.0 ms of it
    the detile). [P] mesa e2f05fca
  - **Vulkan**: no SAND import. The only routes are importing the dma-buf as a
    buffer and de-SANDing in the shader (the engine reads it as `R32UI` and
    `RG32UI` texel buffers), or a copy.
- **HDMI output:** vc4's HDMI connector has `Broadcast RGB`, `max bpc`,
  `Colorspace` and `HDR_OUTPUT_METADATA`. For standard Dolby Vision the
  engine's tunnel frame goes out as 8-bit full-range RGB with a Dolby Vision
  vendor infoframe, which the stock kernel can't send: `patches/linux/` adds
  the `Dolby Vision` connector property (`../ENGINE.md`, "Display output").

## 8. Cheap and expensive on v3dv

The engine's own rules, measured on the Pi, are in `../ENGINE.md` ("What V3D
taught us"). From the facts above:

Cheap:
- **Few jobs.** One render pass, or very few, per frame. Each dispatch costs a
  full drain + IRQ + synchronous cache clean + cold cache (§2).
- **Fragment shaders writing render targets.** Output goes through the on-chip
  TLB with no clean job. Keep all targets ≤ 16 B/px for 64×32 tiles, ≤ 4 B/px
  for 64×64. Integer targets are bit-exact.
- **Few intermediates.** Every one costs a full-frame store and reload out of
  ~11–17 GB/s, and another job.
- **Tables** in `readonly` buffers or UBOs at uniform offsets (`ldunifa`).
- **Float maths, ≤ 32 live registers (4 threads), no spills.**

Expensive:
- dispatch chains with intermediates
- shared memory, barriers
- SSBO and image stores (clean job, write-combiner)
- 32-bit integer multiplies, bitfield builtins, any 64-bit maths
- `uvec4` ALU maths
- sampling linear images
- VkEvents, timestamps inside passes, CPU readback

yblod's desktop-style compute engine hit most of the expensive list, at 515 ms
per picture on an FEL clip.
