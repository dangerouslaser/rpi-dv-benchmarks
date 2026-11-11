# Kodi patches

Standard (TV-led) Dolby Vision output for Kodi's GBM / DRM PRIME path,
rendered by the engine (`libpi_engine`). Against Kodi `c0718c9a69` (the
commit LibreELEC 13 builds), on top of the patches LibreELEC carries for it
(`packages/mediacenter/kodi/patches/*.patch`, `cec-framework/`, and the RPi
series `projects/RPi/patches/kodi/0001–0011`). Apply in order with `git am`.
`../libreelec/0004` carries them as `projects/RPi/devices/RPi5/patches/kodi/`.

| Patch | What |
|---|---|
| `0001` [GBM] Report Dolby Vision from the EDID and the connector | `CDisplayInfo` reads the sink's Dolby Vision data block (libdisplay-info ≥ 0.3.0, `di_cta_data_block_get_dolby_video`). `CWinSystemGbm` reports Dolby Vision when the sink takes standard Dolby Vision and the connector has the `Dolby Vision` property (`../linux/`). |
| `0002` [GBM] Fence the video plane | `CVideoLayerBridgeDRMPRIME::SetVideoPlane` takes a sync_file, committed as the video plane's `IN_FENCE_FD` and closed after the commit; a failed test commit's fallback to the last request replaces that request's fence. |
| `0003` [GBM] Stage the connector state for standard Dolby Vision | `CWinSystemGbm::SetDolbyVision()`: the `Dolby Vision` property, with `Broadcast RGB` Full, `max bpc` 8, no `HDR_OUTPUT_METADATA` and `Colorspace` Default; `Off` restores `Broadcast RGB`. While it is on, the GUI plane starts 8 rows down (scaled into the rest of the screen), clear of the tunnel's metadata packets. The EDID's suggested infoframe: Dolby's for a version 2 data block, else the HDMI one. |
| `0004` [VideoPlayer] DRM PRIME: standard Dolby Vision through libpi_engine | The optional dependency `PiDV` (`cmake/modules/FindPiDV.cmake`, pkg-config `pi_engine`, `ENABLE_PIDV`). `CDVTunnelDRMPRIME` (owned by `CRendererDRMPRIME`) renders each Dolby Vision profile 5, 7 or 8 picture into a tunnel frame, on four XRGB8888 linear buffers from the system DMA heap, and the bridge shows it 1:1, fenced. `VideoPicture::doviProfile` carries the stream's profile. `videoplayer.allowedhdrformats` is offered on builds with the engine (`HAVE_PIDV`). |
| `0005` [VideoPlayer] DRM PRIME: decode the Dolby Vision profile 7 enhancement layer | `CDVELDecoderDRMPRIME` (owned by `CDVDVideoCodecDRMPRIME`): for single-track profile 7 it splits each packet into its layers (`pi_p7_split`), decodes the enhancement layer with a second hardware decoder and pairs the pictures (`pi_pairer`). `CVideoBufferDRMPRIMEFFmpeg` holds the EL frame; the tunnel renders it where the RPU needs it (`pi_frame_needs_el`). |
| `0006` [VideoPlayer] Dolby Vision settings in the video settings dialog | A "Dolby Vision" group in the in-video Video settings, shown when the renderer reports `RENDERFEATURE_DOLBYVISION`: output on / off, enhancement layer (auto / off), infoframe (auto / HDMI VSIF / Dolby VSIF), and at the Expert settings level only: EL upscale (bilinear, the default / bicubic (Catmull-Rom) / Lanczos3 / Annex B), chroma MMR (exact / split / exact 64-bit / float), filters (CB1 / phase). The dialog follows the settings level chosen in Kodi's settings. Changes apply during playback. |
| `0007` [VideoPlayer] DRM PRIME: composite subtitles into the Dolby Vision signal | `CBaseRenderer::RenderOverlays`: a renderer may draw the overlays into the video itself. While the tunnel is used, `CRendererDRMPRIME` draws them into one of three offscreen ARGB8888 dma-bufs of the GUI's size (system DMA heap, an EGL image as a GL framebuffer) and `CDVTunnelDRMPRIME` hands the engine that buffer (`pi_overlay_import`), the area drawn and the drawing's fence (an EGL native fence), so the subtitles are composited into the picture's signal with the whole picture as the active area (L5). `COverlay::GetBounds` gives the area each overlay draws (the GLES glyph and texture overlays: their quads). A change over a paused picture renders it again. Needs the engine's API 2 (0.3.0). |
| `0008` [settings] Add Dolby Vision defaults to Player / Videos | A "Dolby Vision" group in Player / Videos (`HAVE_PIDV`, shown with a Dolby Vision display, enabled while Dolby Vision is an allowed HDR format): `videoplayer.dolbyvisionel`, `videoplayer.dolbyvisioninfoframe`, and at the Expert level `videoplayer.dolbyvisionupscale`, `videoplayer.dolbyvisionmmr`, `videoplayer.dolbyvisionfilters`. `CMediaSettings` keeps the default video settings' options equal to them, every video starts with them (also a file with stored video settings, which don't hold them), and the dialog's "Set as default for all media" stores its options there. |
| `0009` [VideoPlayer] DRM PRIME: composite the GUI into the Dolby Vision signal | While the tunnel is on, `CWinSystemGbm::SetGuiCapture` renders the GUI into a texture (the GBM GLES context's GUI compositing FBO) instead of the GUI plane, which stays transparent, and the renderer copies it (`glBlitFramebuffer`) into the tunnel's overlay buffer, subtitles included (drawn into the GUI, under the OSD), so the OSD and menus are in their own colours. The areas handed to the engine are those of the controls that drew (`CGUIWindowManager::GetRenderedRegions`: a leaf control whose `Render` drew GUI elements) grouped in bands of rows, up to `PI_OVERLAY_RECTS` (the engine's API 3). During playback a GUI change waits for the next picture (a held picture is rendered again after 100 ms), and the GUI is drawn at most 12 times a second (`CApplication::FrameMove`). |
| `0010` [VideoPlayer] DRM PRIME: Dolby Vision for streams cropped to the picture | A stream encoded at the picture's size (3840×2076, say) gets a 16:9 tunnel frame (3840×2160) with the picture in its middle (libpi_engine 0.4.2: an output taller than the stream), so it plays in Dolby Vision in a mode of that size instead of falling back to HDR10. `CDVTunnelDRMPRIME::GetOutputWidth/Height` are the tunnel frame's size, which the renderer checks the mode against. |
| `0011` [dialogs] Player information: Input, Processing and Output columns | Estuary's Player information (`DialogPlayerProcessInfo.xml`, `Includes_PlayerProcessInfo.xml`) in three columns, after the panel of [libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV/pull/92): Input (the stream, its pixel format and colour tags, its Dolby Vision profile, level, CM version and layers, the RPU's levels as chips), Processing (the engine's reconstruction, chroma mapping, filters and GPU time per picture; the static, RPU (L1–L11, L254, L255) and HDR10+ metadata) and Output (the mode, the signal, the tunnel and its infoframe, audio, buffers), and meters. While the dialog is shown (`CDataCacheCore::SetPlayerInfoShown`), `CRendererDRMPRIME` describes the picture on screen twice a second (`DRMPRIME::DescribePicture`, `CDVTunnelDRMPRIME::Describe`) into `CDataCacheCore::SetVideoPlayerInfo`, which the dialog sets as its window properties. The engine profiles every render (`pi_options.profile`). `VideoPicture` carries the configuration record's level and compatibility ID. |

## When the tunnel is used

All of: a Dolby Vision stream of profile 5, 7 or 8 decoded to DRM PRIME
(the direct-to-plane renderer, `videoplayer.useprimerenderer` 0); a Vulkan
device the engine can drive (`pi_probe`: not a CPU implementation, with a
release sync_file); Dolby Vision in "Allowed HDR dynamic metadata formats";
the display reporting Dolby Vision (`0001`); the video setting on. Otherwise
playback is as without the patches. The engine's log messages go to Kodi's
log, prefixed `CDVTunnelDRMPRIME:`; its pipeline cache is
`special://temp/pi_engine.cache`.

## Limitations

- **The mode must be the stream's size,** as the tunnel frame is decoded
  only 1:1: in practice "Adjust display refresh rate" on, with a desktop of
  the stream's size. If the mode stays another size for 100 frames the
  renderer falls back to the decoder's own frames.
- **Subtitles** (text and image) are composited into the picture's
  signal (`0007`), white at 203 nits. They cost the GPU about 2 ms a
  picture while shown (the engine's `docs/ENGINE.md`, Performance), which
  the Annex B EL upscale on 4K FEL doesn't have to spare.
- **The rest of the GUI** (OSD, dialogs, debug overlays) is composited
  over the tunnel frame by the display, below its first 8 rows, which
  carry the metadata packets: the TV stays in Dolby Vision and the GUI is
  legible, but its colours are wrong, and a dimmed dialog background
  shifts the video's colours, since the TV decodes those pixels as
  tunnelled signal.
- **Enhancement layer off** renders MEL pictures without their EL. The
  engine refuses a FEL picture without its EL (`PI_E_UNSUPPORTED_PLAN`), so
  such a picture is rendered with it, and a warning is logged once.
- **The enhancement layer is decoded** whenever the stream has one and the
  tunnel can be used, whatever the settings.
- **The Dolby Vision settings** are kept in the default video settings
  (`guisettings.xml`), not in the video database's per-file settings.
- **Dual-track profile 7** (a separate EL stream) isn't read.
- **Modesets:** a playback start and stop each take two (the refresh rate
  switch, then the connector state), as for HDR10.
- **CMA:** the decoders allocate from CMA. With the enhancement layer, at
  most 12 base layer pictures (6 while no EL picture is held) wait for
  their EL; beyond that the pairer lets them go. The engine's stream is
  reopened at every renderer flush (a seek, the next file), so its imports
  don't keep buffers the decoder has let go of. Opening a Dolby Vision file
  while another one plays can still fail the first pictures' buffer
  allocations (the V4L2 request decoder logs `Failed to get dst buffer`)
  while the previous decoders' buffers are being released.
