# Roadmap — bare-metal MMBasic for Raspberry Pi

## Goal & compatibility target

Deliver an MMBasic interpreter that boots directly on the Raspberry Pi (no
Linux), built from the [PicoMite-fork](https://github.com/marnixk/PicoMite-fork)
MMBasic core running on the [Circle](https://github.com/rsta2/circle) bare-metal
runtime.

The behavioural target is the **Colour Maximite 2 (CMM2)**. Reference manuals:

- [Programming with the Colour Maximite 2](https://geoffg.net/Downloads/Maximite/Programming_with_the_Colour_Maximite_2.pdf)
- [Colour Maximite 2 User Manual](https://geoffg.net/Downloads/Maximite/Colour_Maximite_2_User_Manual.pdf)

## Priorities

1. **Graphics library equivalence (primary).** Match the CMM2 drawing commands
   and screen model as closely as practical.
2. Core language + console I/O (keyboard in, screen/serial out).
3. File system / storage.

Explicitly **optional / deferred** (per project owner): external integrations
such as serial-comms peripherals and Wii / Nunchuck controllers. Missing these
is acceptable.

## Screen model

CMM2 uses `MODE`-selectable resolutions (default 800×600) with multiple
framebuffer `PAGE`s and a `COLOUR`/`RGB()` model. Circle provides an equivalent
foundation: a `CBcmFrameBuffer`-backed display, `CScreenDevice::SetPixel`, and
the double-buffered `C2DGraphics` software library (lines, rectangles, circles,
images, text, VSync). Reaching CMM2 parity means implementing `MODE`,
multi-page framebuffers (`PAGE`), and the `RGB()` colour space on top of these.

## CMM2 graphics command mapping

Status legend: **done** = working in the demonstrator console today,
**planned** = to implement during the port.

| CMM2 command | Syntax (from the manual) | Circle primitive | Status |
| --- | --- | --- | --- |
| `CLS` | `CLS [colour]` | fill framebuffer | done |
| `PIXEL` | `PIXEL x, y, colour` | `SetPixel` / `C2DGraphics::DrawPixel` | done |
| `LINE` | `LINE x1,y1,x2,y2, lw, c` | `C2DGraphics::DrawLine` (Bresenham) | done (no line-width) |
| `BOX` | `BOX x,y,w,h, lw, c [,fill]` | `DrawRectOutline` / `DrawRect` | done (outline only) |
| `CIRCLE` | `CIRCLE x,y,r, lw, a, c, fill` | `DrawCircleOutline` / `DrawCircle` | done (outline only) |
| `COLOUR` | `COLOUR fg, bg` | screen colour state | planned |
| `RGB()` | `RGB(r,g,b)` / `RGB(named)` | `DISPLAY_COLOR(r,g,b)` | planned |
| `TEXT` | `TEXT x,y,string, align, font, scale, c, bg` | `C2DGraphics::DrawText` | planned |
| `FONT` | `FONT n, scale` | Circle fonts (`CFont`) | planned |
| `RBOX` | rounded box | compose primitives | planned |
| `ARC` / `TRIANGLE` / `POLYGON` | filled/outline shapes | `C2DGraphics` + custom | planned |
| `MODE` | `MODE r, bits, bg, int` | framebuffer reconfigure | planned |
| `PAGE` | `PAGE WRITE/COPY n [TO m]` | multiple framebuffers / blit | planned |
| `BLIT` | `BLIT x,y,...` | `DrawImageRect` / buffer copy | planned |
| `IMAGE` / `SPRITE` | image + sprite ops | `DrawImage*` | planned |

The demonstrator today implements `CLS`, `PIXEL`, `LINE`, `BOX`, `CIRCLE` (see
[`console/kernel.cpp`](../console/kernel.cpp)) purely to exercise the graphics
verification path; the real commands come with the MMBasic port.

## Verification strategy

The [QEMU harness](../harness/qemu_harness.py) already supports everything the
graphics work needs:

- inject keystrokes and read exact serial output;
- read individual framebuffer pixels (`screen_pixel`) to assert shape/colour;
- compare a whole frame against a golden image (`image_diff_ratio`);
- OCR the text console (`ocr_screen`).

Each CMM2 graphics command gets tests that draw a known figure and assert on
pixels and/or a golden frame, growing into the "large number of tests" that
demonstrate CMM2 equivalence.

## Milestones

1. **Environment + harness** — done (this PR): toolchains, Circle build, QEMU
   harness (serial + screen + graphics), demonstrator console, test suite.
2. **MMBasic core on Circle** — compile the PicoMite-fork interpreter core as a
   Circle app; wire the console (USB keyboard + screen) to MMBasic I/O.
3. **Graphics commands** — implement the CMM2 drawing commands against
   `C2DGraphics`, with per-command tests.
4. **Screen modes & pages** — `MODE`, `PAGE`, `RGB()`/`COLOUR`, fonts/`TEXT`.
5. **Files & remaining language surface**; external integrations last / optional.
