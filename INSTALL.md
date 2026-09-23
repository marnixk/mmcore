# Installing MMBasic on a Raspberry Pi SD card

This image is a **bare-metal** MMBasic console. There is no Linux on the card.
The Pi boots firmware from the FAT partition, loads the kernel, and shows a
short copyright notice, then `Type HELP ME for a short introduction` and the `>` prompt.

Releases ship **four zips** — pick the one that matches your board. Do not mix
files from different zips on the same card.

| Zip | Boards |
| --- | --- |
| `mmcore-console-rpi3-v*.zip` | Raspberry Pi **3**, **3B+**, **3A+** |
| `mmcore-console-pizero2-v*.zip` | Raspberry Pi **Zero 2** (no onboard Wi-Fi) |
| `mmcore-console-pizero2w-v*.zip` | Raspberry Pi **Zero 2 W** (onboard Wi-Fi) |
| `mmcore-console-pi400-v*.zip` | Raspberry Pi **400** (also Pi **4B** / **CM4**) |

The Zero 2 and Zero 2 W zips reuse the same 64-bit BCM2710 kernel as the Pi 3
zip (`kernel8.img`). Zero 2 W includes CYW43436 blobs under `firmware/`; the
non-wireless Zero 2 zip does not.

Type at the HDMI prompt with a USB keyboard (the Pi 400’s built-in keyboard
counts). Serial UART at 115200 8N1 on GPIO 14/15 still works as a second
console. Video goes to HDMI.

Not installing to a Pi? The same interpreter also builds natively for Linux,
macOS, and Windows (`scripts/build-native.sh`). A Linux x86_64 SDL AppImage is
attached to the rolling `linux-native` pre-release:
<https://github.com/marnixk/mmcore/releases/download/linux-native/mmcore-x86_64.AppImage>.
That build runs `.app` packages and sealed TERM sessions from the command line;
see [`docs/native-desktop.md`](docs/native-desktop.md).

DOS-style drives: `A:` is a RAM disk (always present). `C:` is the SD card
slot. USB mass-storage volumes appear as `D:`, `E:`, … as they are enumerated.
`DRIVE` lists them; `CHDIR "C:"` selects the SD card. File commands without a
drive letter use the current drive (boot default `A:`).

Persistent options live in a hidden INI file: `C:/.mmbasic.ini` on the SD
card (survives reboot). If `C:` is missing (typical QEMU run with no SD
image), the same file is written to the `A:` ramdisk so tests still work —
that copy is lost on power-off. `DIR` and `FILES` hide names that start with
`.`; `OPEN` of the exact path still works.

`OPTION` values that should survive reboot (keyboard, colours, tab, break
key, console, search path, function keys, Wi-Fi credentials, …) are saved
when they change. `FACTORY_RESET` restores firmware defaults and rewrites
the INI (including wiping Wi-Fi SSID/PSK) but does not delete `.BAS`
programs.

`OPTION WIFI "ssid","password"` stores credentials and, on a real Pi with
firmware, brings the radio up with WPA2. Bare `OPTION WIFI` scans and prompts
when the radio is present. `OPTIONS WIFI` joins using those stored credentials;
with none stored it reports `?WIFI not configured`. Hardware release zips for
Pi 3 / 3B+ / 4 / 400 / Zero 2 W include `C:/firmware/` (CYW4343x). The Pi Zero 2
(non-W) zip does not. QEMU does not emulate Wi-Fi: connect reports that the
radio is unavailable. The PSK is written to the INI and is not printed on the
serial console. `OPTION WIFI DEBUG ON` prints `[wifi]` progress on HDMI and
serial; the default is off.

## What you need

- The matching release zip from GitHub Releases
- A microSD card (1 GB or larger is plenty)
- An HDMI monitor and HDMI cable
- A USB keyboard (Pi 400: the built-in keyboard)
- Optional: a USB-to-TTL **3.3 V** serial adapter

## 1. Download the release

From the repository’s **Releases** page, download **one** zip:

- Pi 3 family: `mmcore-console-rpi3-v0.1.1.zip`
- Pi Zero 2: `mmcore-console-pizero2-v0.1.1.zip`
- Pi Zero 2 W: `mmcore-console-pizero2w-v0.1.1.zip`
- Pi 400 / Pi 4: `mmcore-console-pi400-v0.1.1.zip`

Unzip it.

### Raspberry Pi 3 zip

| File | Role |
| --- | --- |
| `kernel8.img` | MMBasic console (Circle kernel) |
| `config.txt` | Firmware boot settings |
| `cmdline.txt` | Circle options (`keymap=US`; change to `UK`, `DE`, …) |
| `bootcode.bin` | GPU boot loader (Pi 1–3) |
| `start.elf` | VideoCore firmware |
| `fixup.dat` | Firmware relocation data |
| `LICENCE.broadcom` | Raspberry Pi firmware licence |
| `INSTALL.md` | This document |
| `install-sdcard.sh` | Linux `--bootstrap` / `--update` helper |
| `VERSION.txt` | Build identity |
| `firmware/` | CYW4343x WLAN firmware (`brcmfmac43430-sdio.*` on Pi 3) |

### Raspberry Pi Zero 2 zip

Same BCM2710 `kernel8.img` as the Pi 3 zip. No `firmware/` directory (this
board has no onboard radio).

| File | Role |
| --- | --- |
| `kernel8.img` | MMBasic console (Circle kernel, same as Pi 3) |
| `config.txt` | Firmware boot settings (`[pi02]`) |
| `cmdline.txt` | Circle options (`keymap=US`; change to `UK`, `DE`, …) |
| `bootcode.bin` | GPU boot loader |
| `start.elf` | VideoCore firmware |
| `fixup.dat` | Firmware relocation data |
| `bcm2710-rpi-zero-2-w.dtb` | Device tree (required) |
| `bcm2710-rpi-zero-2.dtb` | Device tree (included when the firmware tree has it) |
| `LICENCE.broadcom` | Raspberry Pi firmware licence |
| `COPYING.linux` | Licence for the device tree binaries |
| `INSTALL.md` | This document |
| `install-sdcard.sh` | Linux `--bootstrap` / `--update` helper |
| `VERSION.txt` | Build identity |

### Raspberry Pi Zero 2 W zip

Same files as the Zero 2 zip, plus `firmware/` for the onboard CYW43436
(`brcmfmac43436-sdio.*` / `brcmfmac43436s-sdio.*`).

### Raspberry Pi 400 zip

| File | Role |
| --- | --- |
| `kernel8-rpi4.img` | MMBasic console (Circle kernel, BCM2711) |
| `config.txt` | Firmware boot settings (`[pi4]` + ARM stub) |
| `cmdline.txt` | Circle options (`keymap=US`; change to `UK`, `DE`, …) |
| `armstub8-rpi4.bin` | Circle ARM stub (FIQ / GIC on Pi 4 / 400) |
| `start4.elf` | VideoCore firmware for Pi 4 / 400 |
| `fixup4.dat` | Firmware relocation data |
| `bcm2711-rpi-400.dtb` | Device tree for Pi 400 |
| `bcm2711-rpi-4-b.dtb` | Device tree for Pi 4B |
| `LICENCE.broadcom` | Raspberry Pi firmware licence |
| `COPYING.linux` | Licence for the device tree binaries |
| `INSTALL.md` | This document |
| `install-sdcard.sh` | Linux `--bootstrap` / `--update` helper |
| `VERSION.txt` | Build identity |
| `firmware/` | CYW4343x WLAN firmware (`brcmfmac43455-sdio.*` on Pi 4 / 400) |

Pi 4 / 400 load firmware from EEPROM, so `bootcode.bin` is not used.

## 2. Write the SD card

The Pi only reads the **first partition**, which must be **FAT16 or FAT32**
(the firmware does not understand ext4/exFAT for boot).

### Linux (recommended)

Unzip the matching release zip, then run `install-sdcard.sh` as root.
`--bootstrap` partitions the card (MBR + one FAT32 volume named `MMBASIC`) and
copies the boot files. **It erases the whole card.** `--update` overwrites
kernel, firmware, and `config.txt` only, so BASIC files already on `C:` stay.

```bash
# Replace sdX with your card (check with lsblk).
unzip mmcore-console-rpi3-v*.zip        # or pizero2 / pizero2w / pi400
sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX
# sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX
# sudo ./install-sdcard.sh --bootstrap --model pizero2w /dev/sdX
# sudo ./install-sdcard.sh --bootstrap --model pi400 /dev/sdX
```

Later kernel updates:

```bash
sudo ./install-sdcard.sh --update --model rpi3 /dev/sdX
```

`./install-sdcard.sh --help` lists `--from` (directory or zip), `--yes`, and
model aliases. The script refuses virtio/nvme/sata disks and the host root
device; pass `--allow-non-removable` only if you are sure.

### Linux (manual)

If you would rather format the card yourself:

```bash
# Replace sdX with your card (check with lsblk). This erases the card.
sudo wipefs -a /dev/sdX
sudo parted /dev/sdX --script mklabel msdos
sudo parted /dev/sdX --script mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F 32 -n MMBASIC /dev/sdX1
sudo mkdir -p /mnt/mmbasic
sudo mount /dev/sdX1 /mnt/mmbasic
```

A GUI disk tool is fine too: partition table **MBR/DOS**, one **FAT32**
partition, then mount it.

### macOS

Use Disk Utility: erase the card as **MS-DOS (FAT)** with **Master Boot
Record**. Or:

```bash
# Replace diskN after checking `diskutil list`
diskutil eraseDisk FAT32 MMBASIC MBRFormat /dev/diskN
```

### Windows

Use [Raspberry Pi Imager](https://www.raspberrypi.com/software/) **or**
File Explorer:

1. Right-click the SD card → Format
2. File system: **FAT32**
3. Allocation unit: default
4. Quick format is fine

If the card is larger than 32 GB and Windows will not offer FAT32, use
[fat32format](http://ridgecrop.co.uk/index.htm?guiformat.htm) or split a
small FAT32 partition with Disk Management.

## 3. Copy the release files

If you used `install-sdcard.sh --bootstrap` or `--update`, skip this section —
the boot files are already on the card.

Otherwise copy **every file from the zip** to the **root** of the FAT partition.
Keep the `firmware/` directory as a subdirectory (Circle loads Wi-Fi blobs
from `C:/firmware/`). Do not nest the kernel in another folder.

### Raspberry Pi 3 card

```
(SD card, FAT)
├── bootcode.bin
├── cmdline.txt
├── config.txt
├── fixup.dat
├── INSTALL.md
├── install-sdcard.sh
├── kernel8.img
├── LICENCE.broadcom
├── start.elf
├── VERSION.txt
└── firmware/
    ├── brcmfmac43430-sdio.bin
    ├── brcmfmac43430-sdio.txt
    └── …
```

Linux example (after the mount in step 2):

```bash
sudo cp kernel8.img config.txt cmdline.txt bootcode.bin start.elf fixup.dat \
           LICENCE.broadcom INSTALL.md install-sdcard.sh VERSION.txt /mnt/mmbasic/
sudo cp -a firmware /mnt/mmbasic/
sudo umount /mnt/mmbasic
```

### Raspberry Pi Zero 2 / Zero 2 W card

```
(SD card, FAT)
├── bcm2710-rpi-zero-2-w.dtb
├── bcm2710-rpi-zero-2.dtb      # when present
├── bootcode.bin
├── cmdline.txt
├── config.txt
├── COPYING.linux
├── fixup.dat
├── INSTALL.md
├── install-sdcard.sh
├── kernel8.img
├── LICENCE.broadcom
├── start.elf
├── VERSION.txt
└── firmware/                   # Zero 2 W zip only
    ├── brcmfmac43436-sdio.bin
    ├── brcmfmac43436-sdio.txt
    └── …
```

Omit `firmware/` on the non-wireless Zero 2 zip.

### Raspberry Pi 400 card

```
(SD card, FAT)
├── armstub8-rpi4.bin
├── bcm2711-rpi-4-b.dtb
├── bcm2711-rpi-400.dtb
├── cmdline.txt
├── config.txt
├── COPYING.linux
├── fixup4.dat
├── INSTALL.md
├── install-sdcard.sh
├── kernel8-rpi4.img
├── LICENCE.broadcom
├── start4.elf
├── VERSION.txt
└── firmware/
    ├── brcmfmac43455-sdio.bin
    ├── brcmfmac43455-sdio.txt
    └── …
```

Linux example:

```bash
sudo cp kernel8-rpi4.img config.txt cmdline.txt armstub8-rpi4.bin \
           start4.elf fixup4.dat bcm2711-rpi-400.dtb bcm2711-rpi-4-b.dtb \
           LICENCE.broadcom COPYING.linux INSTALL.md install-sdcard.sh VERSION.txt /mnt/mmbasic/
sudo cp -a firmware /mnt/mmbasic/
sudo umount /mnt/mmbasic
```

Unmount / eject the card safely.

## 4. Hook up the Pi

1. Insert the SD card.
2. Connect HDMI to a monitor.
   - Pi 400: use the micro-HDMI port **next to USB-C power** (HDMI0).
   - Pi Zero 2 / Zero 2 W: mini-HDMI.
3. Type on a USB keyboard (Pi 400: the built-in keyboard).
4. Serial is optional (second console, same prompt):
   - Adapter GND → Pi pin **6** (GND)
   - Adapter RX  → Pi pin **8** (GPIO14 / TXD)
   - Adapter TX  → Pi pin **10** (GPIO15 / RXD)
   - Use a **3.3 V** adapter only (5 V will damage the Pi)
   - Pi 400: the GPIO header is on the back of the keyboard
5. Power the Pi from the usual USB / USB-C power supply.

On HDMI you should see the banner. Type `PRINT 6*7` and press Enter; it should
print `42`. Keyboard layout defaults to US (`keymap=US` in `cmdline.txt`).

If you use serial as well, open a terminal at **115200 8N1**, no flow control
(for example `minicom -b 115200 -D /dev/ttyUSB0`, PuTTY, or Screen).

## 5. Confirm it started

On HDMI and on serial you should see:

```
Copyright 2011-2026 Geoff Graham
Copyright 2016-2026 Peter Mather
Adapted and extended by Marnix Kok

Type HELP ME for a short introduction.


>
```

Type a command and press Enter on the USB keyboard (or over serial):

```
PRINT 6*7
```

It should print `42` and return to `>`.

Graphics commands draw on HDMI, for example:

```
CLS
BOX 60,120,200,150,GREEN
CIRCLE 440,200,90,CYAN
```

## 6. Running apps

Packaged `.APP` files can be run by name. `OPTION PATH` lists the folders to
search for them (default `A:/APPS/`); a bare name at the prompt finds and runs
`NAME.APP`:

```
PACKAGE "MYGAME.APP", "MYGAME/"
OPTION PATH "A:/APPS/;C:/APPS/"
MYGAME
```

Ctrl+Space opens a quick picker of every `.APP` on the PATH, and the `APPS`
command opens the same list as a home launcher. To start into an app or the
launcher instead of the REPL, use `OPTION BOOT`:

```
OPTION BOOT LAUNCHER       REM power on straight into the app list
OPTION BOOT "MYGAME"       REM power on into MYGAME.APP
OPTION BOOT REPL           REM back to the default prompt
```

The boot setting is stored in `.mmbasic.ini` on the SD card, so it survives a
power cycle. Press Esc in the launcher to reach the REPL.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| ACT LED does not flash, no HDMI | Card not FAT, files not in the partition root, or `config.txt` missing |
| Rainbow splash then black | Wrong kernel for the board, or files mixed from different zips |
| HDMI works, serial garbage | Baud rate not 115200 8N1, or 5 V adapter; swap TX/RX |
| HDMI works, serial silent | `enable_uart=1` is in `config.txt`; GND connected |
| Pi 400 built-in keyboard does nothing | Use the Pi 400 zip (`kernel8-rpi4.img`); wait a second after the banner for USB to enumerate; try `keymap=` in `cmdline.txt` |
| Wrong symbols (`"` vs `@`) | Edit `cmdline.txt`: `keymap=US` (default), `UK`, `DE`, `FR`, `ES`, `IT` |
| USB keyboard on Pi 3 / Zero 2 does nothing | Plug into a USB-A port (Zero 2: micro-USB OTG); hub-only setups can take a moment after the banner |
| Pi 400 no HDMI | Use HDMI0 (port next to USB-C); `hdmi_force_hotplug=1` is in `config.txt` |
| Pi Zero 2 no HDMI | Mini-HDMI fully seated; `hdmi_force_hotplug=1` is in `config.txt` |
| Wi-Fi not available | Card missing `firmware/brcmfmac*.bin`, this is a Zero 2 (non-W) zip, or this is QEMU (no radio). WLAN zips include `firmware/`. |
| Wrong zip | Pi 3 / Zero 2 / Zero 2 W need `kernel8.img` + `start.elf`. Pi 400 needs `kernel8-rpi4.img` + `start4.elf` + `armstub8-rpi4.bin` |

## Building the zips yourself

From a clone of this repository (AArch64 GNU toolchain on `PATH`):

```bash
scripts/package-release.sh
```

That writes:

- `dist/mmcore-console-rpi3-v0.1.1.zip`
- `dist/mmcore-console-pizero2-v0.1.1.zip`
- `dist/mmcore-console-pizero2w-v0.1.1.zip`
- `dist/mmcore-console-pi400-v0.1.1.zip`

Zero 2 / Zero 2 W packaging reuses the Pi 3 hardware kernel (no third Circle
rebuild). Each zip includes `install-sdcard.sh`. Override the version with
`VERSION=0.2.0 scripts/package-release.sh`. Set `FORCE_FIRMWARE=1` to
re-download Raspberry Pi GPU firmware blobs, or `FORCE_WLAN_FIRMWARE=1`
to re-download the CYW4343x Wi-Fi blobs.

To tag and upload a GitHub Release (after choosing a semantic version; default
is a **minor** bump), use the `github-release` skill or:

```bash
scripts/github-release.sh last-version    # currently 0.1.1
scripts/github-release.sh next-minor      # default next: 0.2.0
scripts/github-release.sh publish 0.2.0
```

`scripts/build.sh` alone produces `console/kernel8.img` for QEMU (Pi 3).
`QEMU=0 RASPPI=4 scripts/build.sh` produces `console/kernel8-rpi4.img`.
The release script builds **without** Circle’s `--qemu` flags so SD host and
display DMA match real hardware.
