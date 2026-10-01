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
| tty2 (`Ctrl+Alt+F2`) | Autologin root shell (`Ctrl+Alt+F1` returns to mmcore) |
| ttyS0 (serial) | Autologin root shell at 115200 8N1 |

If mmcore exits, the tty1 session ends and it starts again.

### Power management (laptops)

`acpid` is enabled on the live and installed images. The **power button** runs a
clean shutdown (`/sbin/poweroff`) after syncing disks and unmounting the
`MMCORE` / `MMCORE-SYS` / `MMCORE-DATA` persistence mounts when they are
mounted. **Closing the lid** suspends to RAM (`echo mem > /sys/power/state`);
opening the lid resumes through the kernel with no extra handler.

At the MMBasic prompt, `SHUTDOWN` performs the same style of power-off on the
framebuffer image (and on a Raspberry Pi). The native Linux/macOS/Windows desktop
apps do not shut down hardware; they report that `SHUTDOWN` is unavailable.

The live session is pinned to the media it booted from: GRUB sets its root by
a marker file at the ISO's own root (`mmcore-live-media`) and the kernel
command line carries `mmcore.live=1`, so the live root is always the stick's
squashfs — even when an internal disk has an installed mmcore (see
[Live USB with a disk installed](#live-usb-with-a-disk-installed-dual-boot)).
The live command line never includes `mmcore.sys=`, so a live session cannot
mount an installed system's `C:`.

The live image needs **at least 2 GiB of RAM**. GRUB loads the kernel and a
small initramfs. The root filesystem stays a squashfs on the stick, mounted
with an in-memory overlay, and pages are read as userspace needs them.

### Hardware firmware

The image ships only the firmware a desktop or laptop PC needs, not Alpine's
full `linux-firmware` meta package (which is ~1.1 GiB installed and covers ARM
SoCs, server SmartNICs and embedded/DSL/USB-TV devices). The keep-list is:

- **CPU:** AMD CPU microcode (`amd-ucode`) for late-loadable security fixes.
- **GPUs:** Intel `i915` and `xe` plus the `intel` audio/Bluetooth blobs, AMD
  `amdgpu` and `radeon`, and NVIDIA `nvidia` (nouveau/GSP).
- **Wi-Fi / Bluetooth:** Intel `iwlwifi`, Broadcom `brcm`, MediaTek
  `mediatek`, Realtek `rtw88`/`rtw89`/`rtlwifi`/`rtl_bt`, and Qualcomm Atheros
  `ath10k`/`ath11k`/`ath12k`/`ath6k`/`ath9k_htc` (AR9271 USB)/`qca`.
- **Audio:** Intel Sound Open Firmware (`sof-firmware`) for Chromebooks and
  modern laptops.

The legacy Marvell `libertas`/`mrvl` pair is intentionally omitted. In Alpine
3.20 the two packages declare each other as hard dependencies, and `mrvl` also
ships Marvell Prestera switch-ASIC and Octeon firmware from the server/embedded
classes this keep-list drops, so it cannot be taken as "Wi-Fi only". Its
libertas/mwifiex Wi-Fi is legacy and rare on the supported x86_64 desktop and
laptop hardware, so the ~83 MiB pair is not worth its size. AMD's `amd` SEV
firmware is omitted too: it is virtualization firmware for SEV guests/hosts,
not a framebuffer desktop client.

Intel's `iwlwifi` blobs live in Alpine's uncategorized `linux-firmware-other`
package alongside unrelated legacy blobs, so the builder installs that package
and prunes it down to just the `iwlwifi-*.ucode` files. If your machine needs a
firmware file that is not in the keep-list, install the matching
`linux-firmware-*` package from Alpine into the installed system (or add it to
the builder's list) -- for example `linux-firmware-mrvl` for an older Marvell
Wi-Fi card.

The builder verifies every package in the keep-list is actually present in the
rootfs after installing it, and **fails the build** if one is missing, so a
typo or an Alpine package rename cannot silently ship an image without that
firmware.

### Slow USB sticks (ThinkPad T420 and similar)

A stick that was fine on a Raspberry Pi is often tuned for large sequential
transfers and answers the small reads from a PC firmware USB stack very
slowly. The ThinkPad T420 makes that worse: its BIOS reads USB at USB 2.0
speeds even from the blue USB 3.0 ports, and its Renesas USB 3.0 controller
stalls on the UAS protocol those sticks advertise.

The image is built for that path. GRUB reads an initramfs that contains the
kernel modules needed to reach the disk. The squashfs uses 1 MiB blocks, which
is the transfer size those drives are good at, and the kernel reads it with
its own USB driver once the firmware is out of the way. The boot command line
blacklists UAS (`modprobe.blacklist=uas`) so the stick stays on the bulk-only
protocol the T420 controller handles. Use a USB 3.0 port when the machine has
one; the kernel reads the squashfs through that port.

Boot is quiet: GRUB hides its menu and auto-boots the `mmcore` entry after
~1 second; hold **Shift** while it counts down to reveal the menu for recovery
or serial debugging. GRUB paints a centered mmcore logo, and the same artwork
is baked into the rootfs and repainted by `mmcore-splash` from the moment the
kernel's framebuffer console takes over tty1 until mmcore modesets its own KMS
surface, so the logo stays on screen for the whole boot instead of a black gap.
The kernel is booted with `quiet loglevel=3` plus a hidden cursor
(`vt.global_cursor_default=0`) so printk and getty banners stay off tty1.
`/dev/console` is the serial port, so OpenRC and `local.d` output go to
`ttyS0`, not the display. Kernel errors and warnings still reach the serial
console, which keeps `ttyS0` useful for debugging. The `MMCORE` persistence
mount and networking bring-up are unaffected.

### Persistent storage

At boot the image looks for a partition labelled `MMCORE` and mounts it at
`/media/mmcore`, exported to mmcore as the drive root (`MMB_DRIVE_ROOT`). With
`install-usb.sh` that partition is created for you; `C:/` then survives
reboots. Without it, files live in RAM and are lost on power-off.

### Install to hard disk

The live USB can also install mmcore onto an internal disk so the machine boots
mmcore on its own. From the live session press `Ctrl+Alt+F2` for a root shell on
tty2 (`Ctrl+Alt+F1` returns to mmcore) and run `mmcore-install`:

```sh
mmcore-install                     # list the disks, then confirm
mmcore-install --disk /dev/sdX     # choose non-interactively (still confirms)
mmcore-install --disk /dev/sdX --yes
```

`mmcore-install` wipes the chosen disk and creates two partitions:

| Partition | Label | Filesystem | Contents |
| --- | --- | --- | --- |
| 1 | `MMCORE-SYS` | FAT32 | GRUB (BIOS + UEFI), the live kernel, the small initramfs, the squashfs root, the `mmcore` binary, and `C:` (`MMB_DRIVE_ROOT`) |
| 2 | `MMCORE-DATA` | ext4 | the rest of the disk, mounted at boot as `D:` |

The system partition is at least ~255 MiB (it grows to fit the kernel, the
initramfs, and the squashfs root). Those files are copied from the live
media's own `/boot`, so the installed system matches the release that wrote
the disk and boots with the USB stick removed.
The installer refuses the running live media and removable disks unless
`--force` is passed; `--boot-dir DIR` reads the boot files from an
already-mounted source instead of auto-detecting it.

Partition 1 is the EFI system partition and the BIOS boot partition. GRUB is
written into the MBR with this disk as the first hard disk, and the UEFI
loader is installed at `\EFI\BOOT\BOOTX64.EFI`. The installer deliberately
does **not** register a named `mmcore` NVRAM entry by default: such an entry
sits ahead of removable media in the firmware boot order, so a machine with
mmcore on the disk and a live USB inserted would boot the installed build
instead of the stick (#976). With no named entry, the firmware falls through
to the removable `\EFI\BOOT\BOOTX64.EFI` path, which a live USB wins when it
is present. Pass `--register-efi` to add the named entry on a firmware that
will not boot `\EFI\BOOT\BOOTX64.EFI` on its own; the disk then boots first
even when a stick is inserted.

After installation the disk boots on its own, on BIOS and UEFI. GRUB mounts
`MMCORE-SYS` as `C:` and launches mmcore with `--drive /media/mmcore-data`, so
`MMCORE-DATA` appears as `D:`.

#### Live USB with a disk installed (dual boot)

The live image is pinned to the stick it booted from, not to the disk. GRUB
sets its root to the stick by the `mmcore-live-media` marker at the ISO root,
and the live kernel command line carries `mmcore.live=1`. `live-init` then
mounts only the marked media; an installed `MMCORE-SYS` partition carries no
marker (the installer copies only `/boot`), so its `/boot/rootfs.squashfs` can
never satisfy a live boot even when the disk is enumerated first. The
installed system's own GRUB still uses `search --label MMCORE-SYS`, which is
correct when the disk boots alone.

The installed boot is pinned the other way around (#979). Its kernel command
line carries `mmcore.sys=LABEL=MMCORE-SYS`, and `live-init` resolves that label
with busybox `findfs` and mounts the squashfs only from that same device.
A live stick enumerated first (`/dev/sda`) therefore cannot supply an installed
boot's root, even though it also has a `/boot/rootfs.squashfs`.

To run the live image with a disk install present, pick the USB from the
firmware boot menu (F12 on a ThinkPad, or the machine's one-time boot key).
With the installer's default (no NVRAM entry), removable media is preferred,
so the USB boots live without changing NVRAM; the installed disk keeps booting
on its own when the stick is removed.

#### Manual dual-boot QA checklist

The QEMU CI smoke boots the ISO alone; it cannot model a disk install next to
a stick. Verify the dual-boot path on hardware (or a two-disk VM):

1. Write the stick and install: `install-usb.sh --iso mmcore-fb-x86_64.iso
   /dev/sdX`, boot it, then run `mmcore-install --disk /dev/sdY --yes` for the
   internal disk.
2. Leave the stick in and boot the machine. It must reach the **live** session
   from the stick. On tty2, `cat /proc/cmdline` must show `mmcore.live=1` and
   no `mmcore.sys=`, and `cat /etc/mmcore-version` must match the stick, not
   the disk's `/media/mmcore-sys/VERSION.txt`.
3. With the stick still in, boot the internal disk from the firmware boot menu
   (or with `--register-efi`). It must reach the **installed** session from the
   disk: `cat /proc/cmdline` shows `mmcore.sys=LABEL=MMCORE-SYS` and no
   `mmcore.live=`, and `/media/mmcore-sys/VERSION.txt` is the disk's — even
   though the stick is the first disk and also carries a
   `/boot/rootfs.squashfs` (#979).
4. Remove the stick and boot the machine alone. It must reach the **installed**
   session; `cat /media/mmcore-sys/VERSION.txt` shows the installed version.
5. Under UEFI, `efibootmgr` must show no `mmcore` entry after a default
   install, and a USB stick inserted at power-on must boot live.
6. Re-run the installer with `--register-efi` and confirm the firmware menu now
   lists `mmcore`; the disk boots when selected (and, as documented, may then
   win over the stick).

#### Update mmcore in place

Once installed, `mmcore-update` fetches the latest published framebuffer build
and replaces just the binary on `MMCORE-SYS` (and `/usr/local/bin/mmcore` for
the running session), so the next boot runs the new version:

```sh
mmcore-update --check             # report installed vs. latest, change nothing
mmcore-update                     # download, verify, install the latest
mmcore-update --version 0.216.0   # install a specific version
mmcore-update --url URL           # install from an explicit tarball/URL
```

It downloads `mmcore-fb-linux-x86_64.tar.gz`, checks that the tarball contains
`mmcore-fb` and `VERSION.txt`, compares the version against the installed one,
and replaces the binary atomically. `MMCORE_REPO`, `MMCORE_BASE_URL` and
`MMCORE_API_URL` override where the release is resolved from. The live image
ships a CA bundle assembled by the ISO builder, so busybox `wget` verifies the
GitHub TLS certificate; the boot smoke test fetches the releases API over HTTPS
to prove it.


### Networking

Ethernet is brought up with DHCP at boot. From the prompt:

```basic
OPTION ETHERNET ON                 ' restart wired DHCP
OPTION WIFI COUNTRY "US"           ' regulatory domain
OPTION WIFI "MyNet", "secret"      ' store and join
OPTION WIFI                        ' join with the stored network
IPCONFIG                           ' show the active interface and address
```

`OPTION WIFI` writes `/etc/wpa_supplicant/wpa_supplicant.conf` and reloads the
running supplicant (`wpa_cli reconfigure`). A helper that does not finish is
killed after a few seconds, and DHCP is a foreground `udhcpc` with a short
retry cap, so a stuck radio returns to the prompt. See
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
package lacks the driver), packs the rootfs as a squashfs plus a small
initramfs that mounts it, and makes a hybrid BIOS+UEFI ISO with
`grub-mkrescue`.

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
  Broadcom, MediaTek and Qualcomm Atheros parts are in the desktop keep-list
  (see [Hardware firmware](#hardware-firmware)).
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
than crash-looping, that Ethernet DHCP comes up, and that the guest trusts the
GitHub TLS certificate (`wget` over HTTPS). Both attach their artifacts to the
published release and to rolling `linux-native` / `linux-iso` pre-releases.
