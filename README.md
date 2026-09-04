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

## Repository layout

| Path | Purpose |
| --- | --- |
| `circle/` | Circle bare-metal runtime (git submodule) |
| `picomite-fork/` | MMBasic interpreter source to be ported (git submodule) |
| `console/` | Bare-metal console app (Circle kernel). Today a placeholder REPL (`PRINT` + CMM2-style graphics); grows into the MMBasic console. |
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

Run it on real hardware by copying `console/kernel8.img` plus the Raspberry Pi
firmware to a FAT SD card (see `circle/boot/`). Run it under QEMU with:

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
must print `5`, and `PRINT "HELLO"` must print `HELLO`. As the MMBasic core is
ported onto Circle, the same pattern extends into a large regression suite for
the real language.

### Demonstrator commands

The placeholder console exists only to exercise the harness end-to-end until
the MMBasic core is ported. It understands:

- `PRINT <expr>` — integer arithmetic (`+ - * /`) or a quoted string
- CMM2-style graphics on the shared framebuffer (verified by pixel and
  golden-image tests):
  - `CLS [colour]`
  - `PIXEL x,y[,colour]`
  - `LINE x1,y1,x2,y2[,colour]`
  - `BOX x,y,w,h[,colour]`
  - `CIRCLE x,y,r[,colour]`

Colours: `WHITE RED GREEN BLUE YELLOW CYAN MAGENTA BLACK`. These map onto the
CMM2 graphics commands tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md).
