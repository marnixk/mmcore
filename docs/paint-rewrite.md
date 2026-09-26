# PAINT rewrite — Dr. Halo / Dr. Genius-style paint app

_Proposal, 2026-09-24. Supersedes the current Deluxe Paint-style
`mmbasic/src/cmd_paint.c`._

## Goal

Replace PAINT with a full-screen, mouse-driven pixel paint application in the
spirit of "Dr. Genius" / "Dr. Halo": a black canvas that fills the screen, a
tool strip down the left, the 256-colour VGA palette across the bottom, and a
sprite-restored cursor that changes shape with the selected tool. Menus are
drawn with the normal text font and driven by the mouse.

## Non-goals (explicitly out of scope)

- Dithering / pattern fills. Solid colours only.
- Editing the palette. It is the fixed default VGA 256 and cannot be changed.
- Persisting settings (tool, colours, window state). None is remembered.
- The old phone/tablet keyboard-only interaction model. PAINT needs a mouse.

## Screen and layout

- **Resolution: 640×360**, exactly 3× 1920×1080 (true 16:9). One canvas pixel is
  one screen pixel.
- **Row 0** is the menu bar (text). It takes its own row; the canvas begins
  directly below it.
- **Left edge**: the tool strip, a **2 × 9 grid** of 32-pixel cells (one icon per
  tool; the ninth row was added for the select tool).
- **Bottom**: the FG/BG indicator at the left end, then the pen-width selector,
  then the palette strip — **4 rows × 64 = 256 swatches**.
- **Canvas** starts entirely black (palette index 0).

## Colour model

- Fixed **default VGA 256** palette (the standard VGA default, not the current
  app's IBM16 + grey + 6×6×6 cube build).
- Not editable; no palette editor.
- PCX files carry the palette, but the app always uses the fixed one.

## Tools

Core (in the first implementation):

- pencil, line, rectangle, ellipse, circle, flood fill, eraser,
  colour pick (eyedropper), grab / custom brush, magnify.

Bonus (may land after the core):

- airbrush, spray, text.

Behaviour:

- **Left button** sets/uses the foreground colour; **right button** the
  background colour (on the palette and on the canvas).
- Line / rectangle / ellipse use **live rubber-band preview** and commit on
  button release. **Shift** constrains to square / circle / 45°.
- **Flood fill is exact-match** (no tolerance).
- **Text** uses a built-in CP437 8×8 font. `Ctrl+F` while typing opens a font
  picker that lists the bitmap fonts in `A:/FONTS/GFX` (a `.json` descriptor
  plus its `.png` sheet); the built-in face stays the fallback and the default.
- **Select** drags out a rectangle; pressing inside it lifts and moves the
  pixels. **Cut / Copy / Paste** and **Del sel** work through a pixel clipboard
  (paste treats the background index as transparent). The boundary is animated
  "marching ants" drawn on screen only, never into the canvas or a saved PCX.

## Cursor

- 32×32 pixel-art sprites, one per tool, in **idle and active** states.
- Implemented with the **sprite-restore** approach so the cursor moves smoothly
  and can cover the interface.
- The cursor is never baked into saved files or screenshots.

## Menus

| Menu | Items |
| --- | --- |
| File | New canvas, Open, Save, Save as, Quit |
| Edit | Undo, Redo, Clear, Cut, Copy, Paste, Del sel, Select |
| Help | Keys |

- Dropdowns: click to open, hover to switch, click-away (or Esc) to close;
  keyboard navigation supported.
- **Confirmation dialogs** for destructive actions (New / Quit with unsaved
  changes, Clear).
- **Open / Save / Save as** use an **EDIT-style file dialog** that can browse and
  write any location (`A:`, `C:`, USB), not just `A:/`.
- **Help → Keys** lists the keyboard shortcuts.

## Undo / redo

- **8 steps** maximum.

## Files

- **PCX** (ZSoft version 5, 8-bit, single plane, RLE, 256-colour palette
  trailer). A good fit for the retro target and palette-preserving.
- The **FILES** browser previews `.PCX` (it currently only previews PNG).

## Lifecycle

- Replaces the current PAINT completely.
- **Refuses to start** with a clear message if no mouse is detected, without
  disturbing the screen. A test-only override lets headless CI/QEMU fake one.

## Testing

- **Primary harness: the native macOS SDL build.** It rebuilds fast and can
  synthesize mouse/keyboard events and hand back the framebuffer for pixel
  assertions (tools, menus, fill, undo, PCX round-trips).
- **Secondary: a thin QEMU smoke test** on the real target (launches, draws,
  saves, exits).

## Module architecture

The rewrite is split into small modules so the parallel issue loop can work on
files that do not overlap. **`paint.h` is owned by the scaffold issue and frozen
first**: it declares every cross-module type and prototype up front so later
issues only add to their own `.c` file.

| File | Owner ticket | Contents |
| --- | --- | --- |
| `mmbasic/src/pcx.c` / `.h` | PCX codec | Encode + decode 8-bit indexed PCX |
| `mmbasic/src/paint_cursor_art.c` / `.h` | Cursor art | 32×32 idle/active art data |
| `mmbasic/src/paint.h` | Scaffold | Shared state + all cross-module prototypes |
| `mmbasic/src/cmd_paint.c` | Scaffold | Lifecycle, layout, event loop, dispatch |
| `mmbasic/src/paint_palette.c` | Palette | VGA palette, swatch strip, FG/BG |
| `mmbasic/src/paint_tools.c` | Tools | Primitives + core tools |
| `mmbasic/src/paint_undo.c` | Undo | 8-step undo/redo |
| `mmbasic/src/paint_menus.c` | Menus | Menu bar, dropdowns, confirm dialogs |
| `mmbasic/src/paint_cursors.c` | Cursor runtime | Sprite-restore cursor, per-tool shapes |
| `mmbasic/src/paint_file.c` | File I/O | Open/Save via the shared file dialog |
| `mmbasic/src/paint_text.c` | Text tool | CP437 text entry + `A:/FONTS/GFX` picker |
| `mmbasic/src/paint_select.c` | Selection | Rectangular select, move, cut/copy/paste |

## Dependency waves

```
Wave 0 (no deps, file-disjoint — start immediately, in parallel)
  A. PCX codec + FILES preview
  B. Cursor art data
  C. Native PAINT test harness + mouse-presence override
  D. PAINT scaffold: screen, layout, event loop, frozen paint.h, no-mouse gate

Wave 1 (depend on D; paint.h is already frozen so these are file-disjoint)
  E. Palette strip + FG/BG            (D)
  F. Core drawing tools + primitives  (D)
  G. Undo/redo, 8 steps               (D)
  H. Menu bar + dropdowns + dialogs   (D)
  I. Cursor runtime                   (B, D)

Wave 2 (depend on wave 1)
  J. File Open/Save/Save-as           (A, D)
  K. Airbrush / spray / magnify       (F; edits paint_tools.c — serialize with F)
  L. Text tool                        (E, F)
  M. Text font picker A:/FONTS/GFX    (L, #625)   [landed #643]
  N. Selection tools + clipboard                 [landed #644]
```

Bundle guidance for the parallel coordinator: A, B, C, D are mutually
file-disjoint and can run concurrently. E–I own separate files and can run
concurrently once D is merged. J can start after A and D but edits
`cmd_files_ui.c`, so it must not overlap A's edit of the same file (A lands
first). K edits `paint_tools.c` and must serialize with F. L adds one
registration hook in `paint_tools.c`, so it must not overlap F or K.

## Rollout

Implement foundations first, then the scaffold, then the modules, then PCX file
I/O, then the bonus tools. Each ticket lists its own dependencies, files, and
acceptance criteria so any loop order that respects the graph produces a working
app.
