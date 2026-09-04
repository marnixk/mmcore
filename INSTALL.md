# Installing MMBasic on a Raspberry Pi SD card

This image is a **bare-metal** MMBasic console. There is no Linux on the card.
The Pi boots firmware from the FAT partition, loads `kernel8.img`, and shows:

```
MMBASIC-CONSOLE READY
>
```

## What you need

- A **Raspberry Pi 3**, **3B+**, or **3A+** (64-bit / AArch64)
- A microSD card (1 GB or larger is plenty)
- An HDMI monitor and HDMI cable
- A USB-to-TTL **3.3 V** serial adapter (to type commands)
- The release zip `mmbasic-console-rpi3-v*.zip` from GitHub Releases

This first release talks to the keyboard over the **serial UART**, not a USB
keyboard. Video goes to HDMI.

## 1. Download the release

From the repository’s **Releases** page, download:

`mmbasic-console-rpi3-v0.1.0.zip`

Unzip it. You should see:

| File | Role |
| --- | --- |
| `kernel8.img` | MMBasic console (Circle kernel) |
| `config.txt` | Firmware boot settings |
| `bootcode.bin` | GPU boot loader (Pi 1–3) |
| `start.elf` | VideoCore firmware |
| `fixup.dat` | Firmware relocation data |
| `LICENCE.broadcom` | Raspberry Pi firmware licence |
| `INSTALL.md` | This document |
| `VERSION.txt` | Build identity |

## 2. Format the SD card

The Pi only reads the **first partition**, which must be **FAT16 or FAT32**
(the firmware does not understand ext4/exFAT for boot).

### Linux

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

Copy **every file from the zip** to the **root** of the FAT partition.
Do not put them in a subfolder.

The card should look like this:

```
(SD card, FAT)
├── bootcode.bin
├── config.txt
├── fixup.dat
├── INSTALL.md
├── kernel8.img
├── LICENCE.broadcom
├── start.elf
└── VERSION.txt
```

Unmount / eject the card safely.

Linux example (after the mount in step 2):

```bash
sudo cp -a kernel8.img config.txt bootcode.bin start.elf fixup.dat \
           LICENCE.broadcom INSTALL.md VERSION.txt /mnt/mmbasic/
sudo umount /mnt/mmbasic
```

## 4. Hook up the Pi

1. Insert the SD card.
2. Connect HDMI to a monitor.
3. Connect serial (optional but needed to type):
   - Adapter GND → Pi pin **6** (GND)
   - Adapter RX  → Pi pin **8** (GPIO14 / TXD)
   - Adapter TX  → Pi pin **10** (GPIO15 / RXD)
   - Use a **3.3 V** adapter only (5 V will damage the Pi)
4. Power the Pi from the usual USB power supply.

On the PC, open a serial terminal at **115200 8N1**, no flow control
(for example `minicom -b 115200 -D /dev/ttyUSB0`, PuTTY, or Screen).

## 5. Confirm it started

On HDMI and on serial you should see:

```
MMBASIC-CONSOLE READY
>
```

Type a command and press Enter (over serial):

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

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| ACT LED does not flash, no HDMI | Card not FAT, files not in the partition root, or `config.txt` missing |
| Rainbow splash then black | `kernel8.img` missing or not 64-bit; this zip is Pi 3 only |
| HDMI works, serial garbage | Baud rate not 115200 8N1, or 5 V adapter; swap TX/RX |
| HDMI works, serial silent | `enable_uart=1` is in `config.txt`; GND connected |
| Wrong Pi model | This zip is **Pi 3 / 3B+ / 3A+**. Pi 4/5 need different kernels |

## Building the zip yourself

From a clone of this repository (AArch64 GNU toolchain on `PATH`):

```bash
scripts/package-release.sh
```

That writes `dist/mmbasic-console-rpi3-v0.1.0.zip`. Override the version with
`VERSION=0.2.0 scripts/package-release.sh`.

`scripts/build.sh` alone produces `console/kernel8.img` for QEMU. The release
script builds **without** Circle’s `--qemu` flags so SD host and display DMA
match real hardware.
