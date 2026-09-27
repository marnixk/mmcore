# Framebuffer desktop and the bootable USB image

Two ways to run mmcore on an x86_64 PC with no desktop environment:

- **`mmcore-fb`** — the native build that renders SDL straight to the Linux
  framebuffer over DRM/KMS. Install it on an existing Linux system and run it
  from a text VT.
- **The live USB / ISO** — a minimal Alpine Linux image that boots into mmcore
  on the framebuffer, with Ethernet and Wi-Fi, and an optional persistent drive
  for your BASIC files.

Both are built by CI and attached to every GitHub release.

## Framebuffer build (`mmcore-fb`)

See [`native-desktop.md`](native-desktop.md#framebuffer-kmsdrm) for the full
build/run notes. In short:

```bash
make -C native sdl-fb        # -> native/mmcore-fb
./native/mmcore-fb           # run from a text VT (no X11/Wayland)
```

The release tarball `mmcore-fb-linux-x86_64.tar.gz` contains the glibc binary
and a README. It needs SDL2 (with the `kmsdrm` driver), `libdrm`/GBM and ALSA
from your distribution.

## Live USB / ISO

The ISO is Alpine Linux with no X or Wayland. `mmcore` autostarts fullscreen on
the KMS/DRM framebuffer, and its networking is driven from the MMBasic prompt.

### Write it to a USB stick

Releases ship the ISO compressed as `mmcore-fb-x86_64.iso.zst`. Decompress it
first, then write it (`zstd` is in the `zstd` package):

```bash
# Linux only. Overwrites the device.
zstd -d mmcore-fb-x86_64.iso.zst          # -> mmcore-fb-x86_64.iso
sudo ./install-usb.sh --iso mmcore-fb-x86_64.iso /dev/sdX
```

`install-usb.sh` also accepts the compressed `.iso.zst`/`.iso.xz` directly and
decompresses on the fly, so `--iso mmcore-fb-x86_64.iso.zst` works just as
well. It writes the hybrid ISO (BIOS + UEFI) and then adds a GPT partition
labelled `MMCORE`, formatted ext4. Pass `--no-persist` to skip it. The same ISO
can be burned to a DVD or written with any hybrid-ISO tool; without the `MMCORE`
partition the session is read-only.

### Boot behaviour

| Console | What happens |
| --- | --- |
| tty1 (display) | Autologin root, then `mmcore` runs fullscreen on the framebuffer |
| tty2 (`Alt+F2`) | Autologin root shell |
| ttyS0 (serial) | Autologin root shell at 115200 8N1 |

If mmcore exits, the tty1 session ends and it starts again.

Boot is quiet: GRUB hides its menu and auto-boots the `mmcore` entry after
~1 second; hold **Shift** while it counts down to reveal the menu for recovery
or serial debugging. During GRUB (and, with `gfxpayload=keep`, as far as the
kernel's framebuffer console allows) the graphical console shows a centered
mmcore logo instead of boot text, and the kernel is booted with
`quiet loglevel=3` plus a hidden cursor (`vt.global_cursor_default=0`) so
printk and getty banners stay off tty1. `/dev/console` is the serial port, so
OpenRC and `local.d` output go to `ttyS0`, not the display. Kernel errors and
warnings still reach the serial console, which keeps `ttyS0` useful for
debugging. The `MMCORE` persistence mount and networking bring-up are
unaffected.

### Persistent storage

At boot the image looks for a partition labelled `MMCORE` and mounts it at
`/media/mmcore`, exported to mmcore as the drive root (`MMB_DRIVE_ROOT`). With
`install-usb.sh` that partition is created for you; `C:/` then survives
reboots. Without it, files live in RAM and are lost on power-off.

### Networking

Ethernet is brought up with DHCP at boot. From the prompt:

```basic
OPTION ETHERNET ON                 ' restart wired DHCP
OPTION WIFI COUNTRY "US"           ' regulatory domain
OPTION WIFI "MyNet", "secret"      ' store and join
OPTION WIFI                        ' join with the stored network
IPCONFIG                           ' show the active interface and address
```

`OPTION WIFI` writes `/etc/wpa_supplicant/wpa_supplicant.conf` and restarts the
OpenRC `wpa_supplicant`/`networking` services, so the same commands work on any
OpenRC system that has these packages. See
[`native-desktop.md`](native-desktop.md#network) for the Linux backend details
and the `MMB_NET_*` overrides.

### Build the ISO

The build runs in an `alpine:3.20` (amd64) container, so the host only needs
Docker:

```bash
scripts/build-iso.sh                 # -> dist/mmcore-fb-x86_64.iso
zstd -19 dist/mmcore-fb-x86_64.iso   # -> dist/mmcore-fb-x86_64.iso.zst
```

The published asset is the `.zst`; CI compresses the ISO after boot-smoking it.

On an x86_64 Alpine Linux host with `apk`, `ISO_DIRECT=1 scripts/build-iso.sh`
runs the builder in place. The builder builds the Alpine rootfs, builds
`mmcore-fb` against musl (building SDL2 with `-DSDL_KMSDRM=ON` if the Alpine
package lacks the driver), packs the rootfs as the initramfs, and makes a hybrid
BIOS+UEFI ISO with `grub-mkrescue`.

`scripts/iso/build-in-container.sh` builds the rootfs and ISO;
`scripts/iso/build-mmcore.sh` builds the binary; `scripts/iso/rootfs-overlay/`
adds the autostart, persistence and networking configuration.

## Intel Chromebooks

The x86_64 ISO can boot on most Intel Chromebooks once the firmware is
unlocked, but some rootfs pieces differ from a generic PC. These are
configuration changes only — the image stays the single hybrid BIOS+UEFI ISO.

### Firmware prerequisites

Stock Chromebook firmware (coreboot + depthcharge) only boots Google-signed
ChromeOS kernels, so **no ISO boots as-is**. The device must first be switched
to **developer mode**, and then either:

- use the **`RW_LEGACY`** slot (SeaBIOS/edk2) bootloader, or
- flash **MrChromebox coreboot + edk2** (the `UEFI (Full ROM)` firmware).

With either one running, the hybrid ISO boots through the GRUB UEFI path.
Installation does **not** bypass verified boot: the firmware unlock is a
deliberate, user-initiated change, and nothing in the ISO touches the
Google-signed firmware or the write-protect state.

### What is supported

- **Display.** Panel output comes up through `i915` (already loaded) and the
  KMS/DRM path mmcore uses on any other Intel GPU.
- **Wi-Fi.** Intel parts use `iwlwifi` (firmware included); the common
  Broadcom/MediaTek/Qualcomm parts are covered by the extra
  `linux-firmware-*` packages the builder installs.
- **Storage.** eMMC (`dw_mmc`/`sdhci`) and NVMe are in Alpine's `linux-lts`.
- **Keyboard.** The internal keyboard is handled by the embedded controller
  (`cros_ec`) plus `atkbd`. The top row emits **F1–F12** (there are no media
  keys unless you hold Fn), and the **Search** key replaces **Caps Lock**;
  mmcore's function-key shortcuts therefore line up with F1–F12.
- **Touchpad / touchscreen.** `cros_ec`, `i2c_hid_acpi`/`i2c_hid_of` and
  `hid_multitouch` are loaded at boot.
- **Audio.** `sof-firmware` plus the `snd_sof*` modules are installed for
  Sound Open Firmware devices.

### Known gap: 32-bit (IA32) UEFI models

Bay Trail, Cherry Trail and some Braswell Chromebooks expose **32-bit UEFI**
even though the CPU is 64-bit. Alpine's GRUB and the x86_64 `linux-lts` do not
provide an IA32 EFI stub for free; booting them would need a separate build
variant with an IA32 GRUB EFI stub and a 32-bit-EFI-capable kernel. Those
models are **unsupported** for now — the ISO does not ship an IA32 variant.

Validation note: the QEMU smoke test cannot reproduce `cros_ec`, panel timing,
SOF audio or the keyboard controller, so the Chromebook configuration above can
only be verified on real hardware.

## CI

`.github/workflows/linux-framebuffer.yml` builds the `mmcore-fb` tarball;
`.github/workflows/linux-iso.yml` builds the ISO and boots it in QEMU on a
virtio-gpu (KMS/DRM) device with a serial console, asserting that it reaches a
shell, that `/dev/dri/card0` exists, that mmcore starts and stays up rather
than crash-looping, and that Ethernet DHCP comes up. Both attach their
artifacts to the published release and to rolling `linux-native` /
`linux-iso` pre-releases.
