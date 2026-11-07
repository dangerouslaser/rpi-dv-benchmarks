# Kernel patches

Against the Raspberry Pi kernel, `raspberrypi/linux` `rpi-6.18.y`
(`a4985478de14`, the source of Raspberry Pi OS's 6.18.60 kernel).

| Patch | What |
|---|---|
| `0001-drm-vc4-hdmi-optionally-send-the-Dolby-Vision-VSIF.patch` | `vc4` module parameter `dv_vsif`: send the Dolby Vision (TV-led) HDMI vendor infoframe instead of the standard one, from the next modeset |

A TV switches to Dolby Vision only with that infoframe; the engine's frames
alone (RGB full range, 8 bpc) are shown as SDR RGB.

**Building the module for a running Raspberry Pi OS kernel** (out of tree,
with `linux-headers-$(uname -r)`): copy `drivers/gpu/drm/vc4` at that commit,
apply the patch, then in `vc4_trace.h` set `TRACE_INCLUDE_PATH` to `.` and add
`CFLAGS_vc4_trace_points.o := -I$(src)` to its `Makefile`, and:

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD CONFIG_DRM_VC4=m modules
# swap the running driver (not persistent: a reboot restores the stock one)
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind
echo axi:gpu | sudo tee /sys/bus/platform/drivers/vc4-drm/unbind
sudo rmmod vc4 && sudo insmod vc4.ko dv_vsif=1
```
