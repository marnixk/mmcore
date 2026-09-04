# raspberrypi-mmbasic

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

The bare-metal build target is a Raspberry Pi `kernel8.img` (AArch64), which is
also directly runnable under QEMU.

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
| `console/` | Bare-metal console app (Circle kernel) hosting the interpreter |
| `harness/` | Python QEMU test harness (keystroke injection, serial + screen reads) |
| `tests/` | Pytest regression suite driving the console under QEMU |
| `scripts/build.sh` | Idempotent build of the Circle core lib + console image |
| `.cursor/` | Cloud Agent environment (toolchains, QEMU, OCR, Python) |

## Toolchain

Circle bare-metal images are built with ARM **freestanding** cross-toolchains
(not the host's Linux gcc), pinned to the Circle-tested ARM GNU **15.2.Rel1**:

- `aarch64-none-elf` — 64-bit bare-metal (Raspberry Pi 3/4/5) — primary target
- `arm-none-eabi` — 32-bit bare-metal and Pico SDK builds

## Building

```bash
scripts/build.sh          # produces console/kernel8.img (RPi3 AArch64, QEMU)
```

Run it on real hardware: see [`INSTALL.md`](INSTALL.md). In short, copy the
files from a GitHub **Release** zip onto a FAT32 SD card (Pi 3 / 3B+ / 3A+).
You can also copy `console/kernel8.img` plus Raspberry Pi firmware yourself
(see `circle/boot/`). Run it under QEMU with:

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
- files: `CHDIR`, `DIR`/`FILES`, `MKDIR`, `RMDIR`, `COPY`, `RENAME`/`NAME`,
  `KILL`, `OPEN`/`CLOSE`, `PRINT #`, `INPUT #`, `SEEK`, `SAVE`
- `LOAD PNG` / `LOAD JPG`, `PLAY MP3` / `PLAY MODFILE` / `PLAY XM`
- `EDIT "file.bas"` — nano-like (`Ctrl+O` write, `Ctrl+X` exit, `Ctrl+R` run)

Colours: `WHITE RED GREEN BLUE YELLOW CYAN MAGENTA BLACK` and `RGB(r,g,b)`.
See [`docs/ROADMAP.md`](docs/ROADMAP.md).
