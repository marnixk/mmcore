# Roadmap — bare-metal MMBasic for Raspberry Pi

_Last reviewed: 2026-09-22._

## Goal & compatibility target

Deliver an MMBasic interpreter that boots directly on the Raspberry Pi (no
Linux), built from the [PicoMite-fork](https://github.com/marnixk/PicoMite-fork)
MMBasic core running on the [Circle](https://github.com/rsta2/circle) bare-metal
runtime.

The behavioural target is the **Colour Maximite 2 (CMM2)**. Reference manuals:

- [Programming with the Colour Maximite 2](https://geoffg.net/Downloads/Maximite/Programming_with_the_Colour_Maximite_2.pdf)
- [Colour Maximite 2 User Manual](https://geoffg.net/Downloads/Maximite/Colour_Maximite_2_User_Manual.pdf)

## Status at a glance

The core port is **shipped**: the interpreter, CMM2 graphics/screen model,
files, audio, networking, full-screen TUIs, and a native Linux backend are all
in the tree and covered by the test suite (releases through **v0.187.0**).
Remaining work is long-tail parity and polish, tracked as open issues, not
greenfield.

| Area | State |
| --- | --- |
| MMBasic core on Circle | done |
| Console I/O (USB keyboard, HDMI, serial) | done |
| CMM2 graphics (MODE / PAGE / COLOUR / RGB / drawing / blit / sprites) | done; edge parity open (#487, #489) |
| Fonts / `TEXT` / `FONT` and TUIs (EDIT, FILES, WORDPAD, HELP, AFK) | done |
| Filesystem (`A:` ramdisk, `C:` SD, USB drives), `PACKAGE` / `.APP` | done |
| Audio (`PLAY MP3/MOD/XM/TONE`) | done |
| Networking (Wi-Fi, Ethernet, TCP, `TERM`, `CONNECT`, FTP server) | done |
| Native desktop backend (`native/mmbasic`, `mmbasic-sdl`, AppImage/Windows zip) | done; hardening open (#486) |
| AppImage/CLI app-VM launch of `.APP` and `TERM` | open (#490, #491) |

Priorities:

1. **Graphics-library equivalence (primary).** Match the CMM2 drawing commands
   and screen model as closely as practical.
2. Core language + console I/O.
3. File system / storage.

Explicitly **optional / deferred** (per project owner): external integrations
such as serial-comms peripherals and Wii / Nunchuck controllers, and
PicoMite-only peripherals (I2C/SPI device ports, camera). Missing these is
acceptable.

## Screen model

CMM2 uses `MODE`-selectable resolutions (default 800×600) with multiple
framebuffer `PAGE`s and a `COLOUR`/`RGB()` colour model. mmcore implements this
on Circle's `CBcmFrameBuffer` + software rasterisers, with HDMI-native pixels
(`COLOR16`, 5-5-5) and RGB888 only at the API boundary.

`MODE r, bits` (modes 1–17, bitdepths 8/12/16/32), multi-page framebuffers,
page-1 transparency/overlay compositing, and `RGB()`/`COLOUR` are all
implemented. See [`graphics-acceleration.md`](graphics-acceleration.md) for the
present/DMA/double-buffer paths and the CPU-only leftovers.

## CMM2 graphics command mapping

Status legend: **done** = implemented and tested; **partial** = implemented with
a known parity gap; **planned/deferred** = not implemented (follow the linked
issue).

| CMM2 command | Syntax (from the manual) | Status |
| --- | --- | --- |
| `CLS` | `CLS [colour]` | done |
| `PIXEL` | `PIXEL x, y, colour` | done |
| `LINE` | `LINE x1,y1,x2,y2[,lw[,c]]` | partial (even-width, `lw>7` — #487) |
| `BOX` | `BOX x,y,w,h[,lw][,c][,fill]` + logic ops | partial (#487) |
| `RBOX` | `RBOX x,y,w,h[,r][,c][,fill]` | partial (default radius — #487) |
| `CIRCLE` | `CIRCLE x,y,r[,lw][,a][,c][,fill]` | partial (aspect ratio — #487) |
| `ARC` | `ARC x,y,r1,r2,a1,a2,c` | done (tessellated) |
| `TRIANGLE` | `TRIANGLE x1,y1,x2,y2,x3,y3[,c[,fill]]` | partial (fill order — #487) |
| `POLYGON` | `POLYGON n, xarray%(), yarray%()…` | partial (inline form; array syntax — #487) |
| `COLOUR` | `COLOUR fg, bg` | done |
| `RGB()` | `RGB(r,g,b)` / `RGB(named)` | done |
| `TEXT` | `TEXT x,y,string, align, font, scale, c, bg` | done |
| `FONT` | `FONT n, scale` | done |
| `MODE` | `MODE r, bits, bg, int` | done |
| `PAGE` | `PAGE WRITE/COPY/DISPLAY/SCROLL/…` | partial (`,B`/`,t` semantics — #487) |
| `BLIT` | `BLIT x,y,w,h[,page][,orientation]`, READ/WRITE/CLOSE | done (transparent/logic CPU — #487) |
| `IMAGE` / `SPRITE` | load/blit + sprite ops | partial (sprite command surface — #487) |

`docs/help/*.txt` is the authoritative per-command reference (198 topics,
compiled into HELP).

## Verification strategy

The [QEMU harness](../harness/qemu_harness.py) boots the kernel under
`qemu-system-aarch64` and drives it like a user:

- inject keystrokes over the emulated PL011 serial console;
- read serial output back for exact, deterministic assertions;
- read the emulated HDMI screen (monitor framebuffer capture) and OCR it.

Each CMM2 graphics command has tests that draw a known figure and assert on
pixels and/or a golden frame. The suite in `tests/` also covers the language
surface, files, audio codecs, networking, TUIs, and the native backend
(`tests/test_linux_native.py`). Run it with `.venv/bin/python -m pytest` (see
[`native-desktop.md`](native-desktop.md) for the host-native fast loop).

## Milestones

1. **Environment + harness** — done: toolchains, Circle build, QEMU harness,
   test suite.
2. **MMBasic core on Circle** — done: interpreter runs on the console with USB
   keyboard + HDMI/serial I/O.
3. **Graphics commands** — done: CMM2 drawing commands against the shared
   rasterisers, with per-command tests.
4. **Screen modes & pages** — done: `MODE`, `PAGE`, `RGB()`/`COLOUR`,
   fonts/`TEXT`.
5. **Files, network, audio, TUIs, Linux native** — done.
6. **Remaining parity & polish** — open: drawing edges (#487), Xmas page-1
   overlay performance (#489), Linux-native fast loop (#486), AppImage/CLI
   app-VM launch (#490, #491).

Circle upstream tickets for the vendored pre-build patches are tracked in
#285 (not ready).
