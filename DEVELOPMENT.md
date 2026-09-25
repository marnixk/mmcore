# Developing mmcore

Architecture, build, packaging, and testing notes for contributors. For what
mmcore is and how to install it, see [`README.md`](README.md).

A bare-metal port of **MMBasic** to the Raspberry Pi. The goal is an MMBasic
interpreter that boots straight on the Pi — no Linux underneath — by combining:

- **[Circle](https://github.com/rsta2/circle)** — a C++ bare-metal environment
  for the Raspberry Pi (screen, USB, serial, timers, …), used as the runtime.
  Vendored as the [`circle/`](circle) submodule.
- **[PicoMite-fork](https://github.com/marnixk/PicoMite-fork)** — the MMBasic
  interpreter source (currently targeting the RP2040/RP2350 Pico). Vendored as
  the [`picomite-fork/`](picomite-fork) submodule; its language core is the code
  being ported onto Circle.

The behavioural compatibility target is the **Colour Maximite 2 (CMM2)**, with
graphics-library equivalence as the priority — see [`docs/ROADMAP.md`](docs/ROADMAP.md).

The default build target is a Raspberry Pi 3 `kernel8.img` (AArch64), which is
also directly runnable under QEMU. Hardware releases also ship that same kernel
for Pi Zero 2 / Zero 2 W, plus a Pi 400 / Pi 4 `kernel8-rpi4.img`.

MMBasic itself is **local code** in [`mmbasic/`](mmbasic). The
[`picomite-fork/`](picomite-fork) submodule is an upstream reference only;
codecs and command behaviour are copied into `mmbasic/` as needed. Circle stays
a submodule.

## Repository layout

| Path | Purpose |
| --- | --- |
| `circle/` | Circle bare-metal runtime (git submodule) |
| `picomite-fork/` | Upstream MMBasic reference submodule (not compiled) |
| `mmbasic/` | Local MMBasic interpreter, commands, and vendored codecs |
| `ramdisk/` | Versioned A: ramdisk seed tree, embedded at build time (see `ramdisk/README.md`) |
| `console/` | Bare-metal console app (Circle kernel) hosting the interpreter |
| `native/` | Native Linux/macOS/Windows SDL2 backend (`mmbasic`, `mmcore`) — see `docs/native-desktop.md` |
| `harness/` | Python QEMU test harness (keystroke injection, serial + screen reads) |
| `tests/` | Pytest regression suite driving the console under QEMU |
| `scripts/build.sh` | Idempotent build of the Circle core lib + console image |
| `scripts/build-native.sh` | Build the host-native (Linux/macOS) backend binaries |
| `scripts/build-windows.sh` | Build the native Windows (MinGW-w64) backend binaries |
| `scripts/package-linux-appimage.sh` | Package `native/mmcore` as a Linux AppImage |
| `scripts/package-macos-app.sh` | Package `native/mmcore` as a signed universal (arm64 + x86_64) macOS `.app` |
| `scripts/package-windows.sh` | Package `native/mmcore.exe` and its DLLs as a signed Windows zip |
| `scripts/sign-windows-exe.sh` | Authenticode-sign a Windows executable (signtool/osslsigncode) |
| `scripts/package-release.sh` | Hardware Pi 3, Zero 2 / 2W, and Pi 400 SD-card zips in `dist/` |
| `scripts/install-sdcard.sh` | Linux `--bootstrap` / `--update` writer for a real SD device |
| `scripts/github-release.sh` | Semantic GitHub release helper (used by the `github-release` skill) |
| `docs/` | Roadmap plus `docs/help/` topic files compiled into HELP |
| `.cursor/` | Cloud Agent environment (toolchains, QEMU, OCR, Python) |

## Toolchain

Circle bare-metal images are built with ARM **freestanding** cross-toolchains
(not the host's Linux gcc), pinned to the Circle-tested ARM GNU **15.2.Rel1**:

- `aarch64-none-elf` — 64-bit bare-metal (Raspberry Pi 3/4/5) — primary target
- `arm-none-eabi` — 32-bit bare-metal and Pico SDK builds

## Building

```bash
scripts/build.sh                         # console/kernel8.img (RPi3, QEMU)
QEMU=0 RASPPI=4 scripts/build.sh         # console/kernel8-rpi4.img (Pi 400)
scripts/package-release.sh               # hardware zips in dist/
```

`ramdisk/` is embedded and seeded into A: at boot. `RAMDISK_EXCLUDE=tests`
omits a top-level folder (release builds drop the test fixtures).

Run it on real hardware: see [`INSTALL.md`](INSTALL.md). GitHub **Releases**
ship FAT32 SD-card zips for Pi 3 / 3B+ / 3A+, Pi Zero 2, Pi Zero 2 W, and
Pi 400 (also Pi 4B / CM4). On Linux, unzip a release and run `install-sdcard.sh`:

```bash
sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX
sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX
sudo ./install-sdcard.sh --bootstrap --model pizero2w /dev/sdX
sudo ./install-sdcard.sh --bootstrap --model pi400 /dev/sdX
```

Type at HDMI with a USB keyboard (Pi 400: the built-in keyboard). Serial on
GPIO 14/15 is optional.

### Native desktop fast loop

For day-to-day editing without QEMU, build the host-native binaries (Linux,
macOS, or Windows) and run the scoped tests:

```bash
scripts/build-native.sh                                    # native/mmbasic + native/mmcore
.venv/bin/python -m pytest tests/test_linux_native.py     # REPL, storage, SDL, TUIs, TCP
```

A prebuilt Linux x86_64 SDL AppImage is attached to the rolling `linux-native`
pre-release: <https://github.com/marnixk/mmcore/releases/download/linux-native/mmcore-x86_64.AppImage>.
`scripts/package-macos-app.sh` builds a signed universal `mmcore.app` bundle
(`dist/mmcore-macos-universal.zip`, arm64 + x86_64), also attached to normal
releases. It compiles each slice with `-arch` (see `MACOS_ARCHES`) and downloads
the official universal SDL2 from libsdl.org into `.cache/`, so no Intel
Homebrew is needed.
`scripts/build-windows.sh` / `scripts/package-windows.sh` build a native
Windows x86_64 zip (`dist/mmcore-windows-x86_64.zip`); CI attaches it to every
release and to the rolling `windows-native` pre-release.
See [`docs/native-desktop.md`](docs/native-desktop.md) for the app-VM CLI (`.app`,
`--term`), the Pi-vs-native matrix, and where to escalate to QEMU.

Settings persist in `C:/.mmbasic.ini` on the SD card (`A:/.mmbasic.ini` when
`C:` is missing, e.g. QEMU without an SD image). `FACTORY_RESET` restores
defaults without deleting programs. `OPTION WIFI "ssid","password"` stores credentials and
joins a WPA2 network on a real Pi 3 / 4 / 400 / Zero 2 W (firmware in `C:/firmware/`).
`OPTIONS WIFI` reconnects with stored credentials (`?WIFI not configured` if none).
`OPTION WIFI DEBUG ON` prints `[wifi]` progress on HDMI and serial
(default off; the password is never printed). Hardware images program a
firmware keep-alive on CYW43455/43456 (Pi 4 / 400) as well as 4330.
`OPTION ETHERNET ON` uses the RJ45 port (DHCP) instead of Wi-Fi on boards that have
one (Pi 3 / 3B+ USB LAN, Pi 4 Gigabit). Only one interface is active; Ethernet ON
disables Wi-Fi auto-join, and `OPTIONS WIFI` disables Ethernet. A switch after the
stack is already up needs `REBOOT`. If Ethernet is ON it initialises at boot.
`IPCONFIG` reports the active interface (`Interface: Ethernet` or `Interface: Wi-Fi`)
and prints connected only after a live gateway probe. QEMU has no Wi-Fi radio.
Ethernet under QEMU uses a USB CDC gadget:

```bash
qemu-system-aarch64 -M raspi3b -kernel console/kernel8.img -serial stdio -display none \
  -netdev user,id=net0 -device usb-net,netdev=net0
```

DHCP typically assigns `10.0.2.15` (host is `10.0.2.2`). Without those flags,
`OPTION ETHERNET ON` reports Ethernet not available. The test harness omits
`usb-net` unless a test requests `net_console`.

You can also copy a kernel plus Raspberry Pi firmware yourself (see
`circle/boot/`). Run the Pi 3 image under QEMU with:

```bash
qemu-system-aarch64 -M raspi3b -kernel console/kernel8.img -serial stdio -display none
```

## Automated testing (QEMU harness)

Serious development needs an automated way to prove the interpreter behaves
correctly. The harness ([`harness/qemu_harness.py`](harness/qemu_harness.py))
boots the kernel under `qemu-system-aarch64` and drives it like a user:

- **injects keystrokes** over the emulated PL011 serial console;
- **reads serial output** back for exact, deterministic assertions;
- **reads the emulated HDMI screen** by capturing the framebuffer via the QEMU
  monitor and OCR-ing it (ImageMagick + tesseract).

Run the suite:

```bash
.venv/bin/python -m pytest        # builds the image, then runs the tests
```

Each test types a command and checks the console output — e.g. `PRINT 2+3`
must print `5`, and `PRINT "HELLO"` must print `HELLO`. The suite also covers
types (`$`, `%`, `ARRAY()`), `OPTION` subcommands, graphics `MODE`s, file
commands, JPEG/PNG loaders, MP3/MOD/XM playback, and the nano-style `EDIT`
command.

### Language surface (local MMBasic)

The console runs the local interpreter in `mmbasic/`. Immediate mode at the
`>` prompt understands regular MMBasic plus CMM2-style graphics:

- expressions, `PRINT`, `DIM` / `DIM AS`, typed variables (`A$`, `A%`) and arrays
- control flow: `GOTO`, `GOSUB`/`RETURN`, `FOR`/`NEXT`, `WHILE`/`WEND`, `DO`/`LOOP`,
  `IF`/`THEN`/`ELSE`/`ENDIF`, `SELECT CASE`, `SUB`/`FUNCTION`, `DATA`/`READ`/`RESTORE`, `CONST`
- `OPTION` and CMM2 subcommands (`BASE`, `DEFAULT`, `EXPLICIT`, `ANGLE`, `LIST`, `RESET`, …)
- `MODE r, bits` (modes 1–17, bitdepths 8/12/16/32) and drawing: `CLS`, `PIXEL`, `LINE`,
  `BOX`, `CIRCLE`, `RBOX`, `TRIANGLE`, `POLYGON`, `ARC`, `TEXT`, `FONT`, `COLOUR`, `PAGE`,
  `BLIT`, `RGB()`, `PIXEL()`
- files: `DRIVE`, `CHDIR`, `DIR`/`FILES`, `MKDIR`, `RMDIR`, `COPY`, `RENAME`/`NAME`,
  `KILL`, `OPEN`/`CLOSE`, `PRINT #`, `INPUT #`, `SEEK`, `SAVE`, `PACKAGE`
  (`A:` ramdisk, `C:` SD card, `D:`… USB mass storage; `RUN "name.app"` mounts `B:`)
- `LOAD PNG` / `LOAD JPG`, `PLAY MP3` / `PLAY MODFILE` / `PLAY XM` / `PLAY TONE`
  (`OPTION AUDIO_TARGET HDMI|JACK`)
- `EDIT "file.bas"` — TUI editor (`Ctrl+S` save, `Ctrl+O` outline, `Alt+X` quit, `Ctrl+R` run)

Colours: IBM PC palette 0-31 (`COLOUR 4`, `LIGHTRED`) plus
`WHITE RED GREEN BLUE YELLOW CYAN MAGENTA BLACK` and `RGB(r,g,b)`.
See [`docs/ROADMAP.md`](docs/ROADMAP.md).
