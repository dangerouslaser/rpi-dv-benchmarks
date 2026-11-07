# Kernel patches

Against the Raspberry Pi kernel, `raspberrypi/linux` `rpi-6.18.y`
(`a4985478de14`, the source of Raspberry Pi OS's 6.18.60 kernel). Apply them
in order with `git am`.

| Patch | What |
|---|---|
| `0001-video-hdmi-Add-the-Dolby-Vision-vendor-specific-info.patch` | The HDMI infoframe library packs the two vendor infoframes that signal standard Dolby Vision: the HDMI one padded to 24 bytes, and Dolby's (OUI 00-D0-46) |
| `0002-drm-connector-Add-the-HDMI-Dolby-Vision-property.patch` | The HDMI connector property `Dolby Vision`, with its documentation |
| `0003-drm-display-hdmi-Implement-the-Dolby-Vision-property.patch` | The HDMI state helper implements it: the atomic check, the mode change, the infoframes |
| `0004-drm-tests-Add-tests-for-the-HDMI-Dolby-Vision-proper.patch` | KUnit tests |
| `0005-drm-vc4-hdmi-Attach-the-Dolby-Vision-property.patch` | `vc4` attaches the property to its HDMI connectors |

## The `Dolby Vision` property

In standard (TV-led) Dolby Vision the frames carry the Dolby Vision signal
tunnelled in 8-bit RGB, and the TV does the display mapping. The TV decodes
them only when the source sends a Dolby Vision vendor infoframe; without one
it shows them as SDR RGB. The property is an enum on the HDMI connector:

| Value | Vendor infoframe | AVI infoframe VIC |
|---|---|---|
| `Off` (the default) | the HDMI one, as without the patches | as without the patches |
| `Standard (HDMI VSIF)` | the HDMI one (OUI 00-0C-03), padded with zeros to 24 bytes. It keeps the HDMI_VIC of the HDMI 1.4b 4k modes (3840x2160 at 24, 25 and 30 Hz, 4096x2160 at 24 Hz) | as without the patches: 0 for those 4k modes |
| `Standard (Dolby VSIF)` | Dolby's (OUI 00-D0-46), 27 bytes, PB4 = 0x02 (Dolby_Vision_Signal), the rest zero | the CTA-861 VIC, for those 4k modes too (93 for 3840x2160 at 24 Hz) |

At 3840x2160 24 Hz the vendor infoframe is `81 01 18 34 03 0c 00 20 03`
then zeros (28 bytes) with the HDMI VSIF, and `81 01 1b 4b 46 d0 00 02` then
zeros (31 bytes) with the Dolby VSIF.

**Which value:** sinks with a version 2 Dolby Vision VSVDB in their EDID are
commonly sent the Dolby VSIF, others the HDMI one. The kernel doesn't parse
that EDID block; user-space chooses. An LG B4 (webOS 9.1, a version 2 VSVDB)
takes both.

**The atomic check.** With any value but `Off`, a commit is rejected
(`EINVAL`) unless:
- the output is RGB at 8 bpc: `max bpc` = 8 (this tree's default; above 8,
  vc4 uses deep colour where the link allows it);
- in full range: `Broadcast RGB` = `Full` (`Automatic` gives limited range on
  CTA-861 modes);
- `Colorspace` is `Default` and no `HDR_OUTPUT_METADATA` is set;
- the mode is progressive and not 3D, and the sink is HDMI, not DVI.

With `drm.debug=0x4` the kernel log gives the reason (`Dolby Vision needs
...`).

**Changing it is a modeset**, since vc4 writes the infoframes when it enables
the output. Commit with `DRM_MODE_ATOMIC_ALLOW_MODESET`, best in the same
commit that sets the mode. The value stays in the connector state until it
is changed, also for the console once the DRM master closes, so set it back
to `Off` before handing the output back.

Low-latency (source-led) Dolby Vision is not included.

**What is sent** is in debugfs, as for the other infoframes:
`/sys/kernel/debug/dri/1/HDMI-A-1/infoframes/hdmi` (the vendor slot) and
`.../avi`.

## Building

The patches change the DRM core's connector state and the HDMI infoframe
library, which the Raspberry Pi config builds in (`CONFIG_HDMI=y`). They need
a whole kernel build, not an out-of-tree module. Natively on arm64 (for
example Docker on an Apple silicon Mac, on a case-sensitive filesystem), with
the Pi's own config:

```sh
git init linux && cd linux
git fetch --depth 1 https://github.com/raspberrypi/linux.git a4985478de14b3a83c8e04fed68377c1bdae735d
git checkout FETCH_HEAD && git am /path/to/patches/linux/*.patch
mkdir ../out && cp config-6.18.60+rpt-rpi-2712 ../out/.config   # the Pi's /boot/config-*
scripts/config --file ../out/.config --set-str LOCALVERSION -dv-2712
make O=../out olddefconfig
make O=../out LOCALVERSION= -j$(nproc) Image modules
make O=../out LOCALVERSION= INSTALL_MOD_PATH=../modinst INSTALL_MOD_STRIP=1 modules_install
```

That gives kernel release `6.18.60-dv-2712`, `../out/arch/arm64/boot/Image`
and `../modinst/lib/modules/6.18.60-dv-2712`.

**KUnit**, on arm64 under QEMU (`qemu-system-aarch64`; UML needs an x86
host):

```sh
./tools/testing/kunit/kunit.py run --kunitconfig=drivers/gpu/drm/tests --arch=arm64
```

The Dolby Vision cases are in the `drm_atomic_helper_connector_hdmi_check`,
`drm_atomic_helper_connector_hdmi_reset`,
`drm_connector_attach_dolby_vision_property` and
`drm_hdmi_connector_get_dolby_vision_name` suites. On this base, 10 of the
existing bpc tests in `drmm_connector_hdmi_init` and the HDMI state helper
suites fail with or without the patches: the tree's HDMI connector reset
requests 8 bpc, where those tests expect the connector's maximum.

## Installing on a Raspberry Pi 5

The drivers needed to boot (RP1, USB storage, SD, ext4) are built in, so the
kernel boots without an initramfs and with the stock device trees and
overlays. It goes next to the stock kernel, which stays as it is:

```sh
sudo tar xzf modules.tgz -C /lib/modules/       # 6.18.60-dv-2712/
sudo cp Image /boot/firmware/kernel_2712-dv.img
```

**Trying it once** with the firmware's tryboot: `tryboot.txt` is used for a
single boot, and the next boot is from `config.txt` again, so a kernel that
doesn't come up costs one power cycle at most (with `panic=10` a panic
reboots by itself).

```sh
cd /boot/firmware
sed 's/$/ panic=10/' cmdline.txt | sudo tee cmdline-dv.txt
{ cat config.txt; printf '\n[all]\nkernel=kernel_2712-dv.img\ncmdline=cmdline-dv.txt\nauto_initramfs=0\n'; } | sudo tee tryboot.txt
sudo reboot '0 tryboot'
```

**Making it the default:** the same `[all]` lines at the end of `config.txt`
(the `cmdline` line optional). To go back, delete them: on the Pi, or from
another computer, as the boot partition is FAT.
