# TERM sixel graphics

Analysis only. Nothing in this document is implemented, and no issues have
been filed. TERM is the built-in fullscreen terminal in `mmbasic/src/cmd_term.c`.
The goal is sixel (`DCS` … `q` … `ST`) that `img2sixel`, `chafa` and `lsix`
can drive, painted into the same RGB565 HDMI framebuffer the text pane
already uses.

## Where the code lives

| Piece | Location |
| --- | --- |
| Terminal app, parser, cell grid, scrollback, present | `mmbasic/src/cmd_term.c` |
| 8×16 / 8×14 CP437 glyph blit into a `uint16_t` page | `mmb_gfx_glyph_cell_topdown_h` in `mmbasic/src/gfx.c` |
| Page allocation (`uint16_t` per pixel) | `page_buf` in `mmbasic/src/gfx.c` |
| HDMI pack and DMA present | `rgb_to_raw`, `plat_term_present_async` in `console/platform.cpp` |
| SDL / framebuffer pack (different bit layout) | `sdl_rgb_to_native` in `native/sdl_video.c` |
| Help text that describes today's ANSI subset | `kText_TERM` in `mmbasic/src/help_data.c` |
| QEMU coverage | `tests/test_term*.py`, replay via `TERM REPLAY` |

`picomite-fork/` is an upstream reference only. Sixel belongs in `mmbasic/`,
not in that tree and not in Circle.

Circle stays the bare-metal runtime. The HDMI scanout is 16-bit. On the Pi
the bytes in a graphics page are standard RGB565, packed by `rgb_to_raw`:

```c
((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
```

That is 5 bits red, 6 bits green at bit 5, 5 bits blue. Circle's `COLOR16`
macro is a different layout (5-5-5, green at bit 6, bit 5 clear). Using it
for a direct fill made `CLS` a different shade from the same colour painted
by the terminal (`#1233`). The SDL framebuffer build packs that 5-5-5 layout
in `sdl_rgb_to_native`. TERM must keep calling `mmb_pix_store()` so a sixel
palette entry is the same `uint16_t` a glyph would store on that platform.
Hard-coded RGB565 shifts would be wrong on the SDL framebuffer build, and
`COLOR16` shifts would be wrong on HDMI.

`mmb_pix_store` quantises through `G.gfx.bits` and then calls
`mmb_rgb_to_native`. A TERM session forces at least 16-bit colour
(`term_apply_session_mode`), so the quantiser is the RGB565 one. Pages are
always `uint16_t` (`page_buf`), including when a BASIC program asked for
8- or 32-bit colour. The 8- and 32-bit modes change quantisation, not the
storage width.

## How the screen works today

### Cell grid

Each console has a `tm_state` (`T_s[]`, up to four consoles). The visible
pane is a character grid:

- `cell[TM_MAX_ROWS][TM_MAX_COLS]` — one CP437 byte per cell. `TM_MAX_ROWS`
  is 80, `TM_MAX_COLS` is 256.
- `cell_fg` / `cell_bg` — packed RGB888 (`0xRRGGBB`) per cell.
- Cursor `cur_row` / `cur_col`, pen `cur_fg` / `cur_bg`, bold and inverse.
- Scrolling region `scroll_top` / `scroll_bot` (DECSTBM, inclusive, 0-based).
  Default is the whole pane.

Cells are 8 pixels wide (`TM_CW`). Height is 16 (VGA CP437, `mmb_cp437_8x16`)
or 14 (EGA, `mmb_cp437_8x14`) when an 80×25 view cannot fit 25 rows of 8×16.
`pane_put` writes the CP437 byte and the current pen, then advances the
cursor. At the right margin it calls `pane_newline`. A newline on the bottom
margin scrolls one row (`pane_scroll_up`); anywhere else it only moves the
cursor. A scroll whose top margin is row 0 pushes that row into scrollback.
A DECSTBM region that does not start at row 0 discards the row that leaves
the region.

There is no pixel layer. Anything that is not a cell is gone on the next
repaint of that row.

### Scrollback

`term_sb_state` is a heap ring, one per console, allocated on first TERM use
and freed on leave (`term_session_alloc`, `term_sb_free`). Cap is
`OPTION TERM SCROLLBACK` (default 200, hard max `TM_SB_MAX` 256, 0 disables).
Each slot stores the row's width plus per-cell character, foreground and
background. The struct is on the order of 576 KB. Page Up / Page Down change
`sb_view`; `term_view_cell` reads the ring instead of the live grid. Search
walks the stored characters. Scrollback has no pixels.

### Palette and pen

`ansi_pal` is a fixed 16-colour VGA-ish table (dim red `0xAA0000`, and so
on). `ansi_256` adds the 6×6×6 cube and the grey ramp for `SGR 38;5` /
`48;5`. Truecolour `38;2` is not parsed. Bold lightens the foreground;
inverse swaps pen and paper. Defaults are `TM_FG` `0xAAAAAA` and `TM_BG`
`0x000000`. The pane background is black even when the theme is not. Erase
(`ED`, `EL`, `ECH`, insert/delete) fills with the current paper (background
colour erase).

These SGR colours are unrelated to a sixel palette. A sixel register is a
separate table.

### Drawing and the framebuffer

`term_draw_row` walks a dirty row and, for each cell whose packed
character/colour key changed, calls `term_cell` →
`mmb_gfx_glyph_cell_topdown_h`. That writes an opaque 8×N glyph straight
into page 2 (`TM_PAGE`) as native `uint16_t` pixels. Rows are top-down even
when `OPTION Y_AXIS UP` is set. The shadow key skips unchanged cells inside
a dirty row.

Dirty tracking is a row band (`dirty_lo`/`dirty_hi`) or the whole pane
(`dirty_full`). `term_draw` repaints that band, then one present:

- `term_present_async_rect` hands page 2 to `plat_term_present_async`.
- On the Pi, a rectangle whose width equals the page stride is DMA'd
  straight from page 2 (`SetArea`). A narrower rectangle is copied into a
  bounce buffer first, because the DMA source has to be tightly packed.
- QEMU is built with `NO_SCREEN_DMA_BURST_LENGTH`, so the same call is a
  synchronous `SetArea`. QEMU timings are not Pi timings.
- While a background console drains (`s_term_bg`), the grid updates and
  `need_draw` stays set. Nothing is painted until that console is visible.

`term_draw` presents the full width of the pane for the dirty rows, plus
the status row when the menu or scrollback chrome is up. A burst of
newlines marks the band once and presents once, at the poll point
(`file_replay_poll` / the main poll), not once per line.

Menu, dialog and status drawing call `term_cell` directly into page 2.
Closing the menu does `mark_dirty_full` and rebuilds the pane from the
cell arrays. Page 2 is a cache of the grid, not the source of truth.

### Size of the pane

`term_layout` sets the grid from the framebuffer:

- Boxed (default): MODE 14, 960×540, 80 columns centred, `pane_rows` is
  `video_rows - 1` (32 rows of 8×16). Pixel area of the pane is
  640×512.
- Full: the mode TERM was started from, no letterbox, width capped at
  256 columns.
- 80×25: MODE 2, 640×400, when the display is tall enough. Short panels
  (MODE 20, 683×384) keep their mode and use the 8×14 face.

View → Full → Boxed → 80×25 calls `term_cycle_view`: it may change MODE
(which reallocates pages), resets DECSTBM, blanks new columns, clamps the
cursor, clears page 2 and page 0, and sends NAWS if TCP is up. Cell text
survives a resize. There are no pixels to keep.

### What the parser accepts

`incoming_byte` strips telnet IAC, then `ansi_feed`. `term_rx_interpret`
also watches the raw byte stream for ZMODEM `* * CAN` before the ANSI
state machine sees the byte.

`ansi_feed` states:

| `ansi_st` | Meaning |
| --- | --- |
| 0 | Ground. `ESC` (0x1B) enters state 1. |
| 1 | After `ESC`. `[` CSI, `]` OSC, `7`/`8` save/restore cursor, `c` RIS, `( ) * + -` charset (one following byte, state 7). |
| 2 | CSI parameters. `?` sets the private flag. Digits and `;`. Final byte `0x40..0x7E` dispatches. |
| 4, 5 | OSC, discarded until BEL or `ESC` plus one byte. Idle abort `TM_ANSI_OSC_MS` (2 s). |
| 7 | Swallow one charset byte. |

CSI dispatch (`ansi_exec_csi` / `ansi_exec_priv`) covers cursor movement,
CUP, ED/EL/ECH, IL/DL, ICH/DCH, REP, SU/SD, DECSTBM, SGR, save/restore
cursor, DSR (CPR and status), cursor visibility (`CSI ? 25 h/l`), and
primary DA.

Primary DA (`CSI c`) replies `\x1b[?1;2c` — VT100 with AVO. The reply is
sent only when `T.tcp` is set (`ansi_reply`). Demo and file replay do not
answer. There is no parameter `4`.

`CSI` final bytes `t` and `S` are ignored. `ansi_exec_priv` ignores every
private mode except 25. XTWINOPS, XTSMGRAPHICS and DECSDM (mode 80) are
not implemented. NAWS (telnet option 31) already reports the pane in
characters. `TTYPE` is the string `ANSI`.

Parameter storage is `TM_ANSI_ARGS` (8) integers. A ninth `;` does not
grow the list; further digits accumulate into the last slot.

The other escape buffer, `esc_buf[TM_ESC_BUF]` (16 bytes) and `esc_feed`,
is the keyboard path that forwards local cursor keys to the host. It is
not the receive parser.

### DCS is not handled

`ESC P` (0x50) hits the unknown-introducer branch in state 1: the `P` is
consumed and the state returns to ground. The sixel body is then ordinary
text. `pane_put` prints every byte `>= 32`, so `q`, `#`, digits, `!`,
`$`, `-` and the sixel characters are written into cells. The `ESC` of
the string terminator `ESC \` is eaten as another unknown escape, and the
backslash with it.

A concrete stream:

```
ESC P 0 ; 0 ; 0 q #0;2;100;0;0 ~ ESC \
```

paints the characters `0;0;0q#0;2;100;0;0~` and does not draw a red pixel.

8-bit C1 is not recognised either. `DCS` (0x90) is below 32 and is
dropped; the body would print. `OSC` (0x9D), `CSI` (0x9B) and `ST`
(0x9C) are in the same position. `img2sixel` defaults to 7-bit `ESC P` /
`ESC \`, which is the form that matters.

Until a DCS state exists, any partial sixel work will spew the payload
across the pane. The first change has to swallow `ESC P` … `ST` even
for DCS commands that are not sixel.

## DCS and the sixel decoder

Keep the decoder out of `cmd_term.c`. A small `mmbasic/src/sixel.c` that
consumes one byte at a time can be compiled into a host test the same way
`tests/test_paint_damage.py` compiles `cmd_paint.c`. TERM owns the grid,
the plane and the replies. The decoder owns the grammar and writes native
pixels into a buffer the caller provides.

Do not accumulate the DCS in `esc_buf`. Sixel is decoded as it arrives.
The 512 KB RX ring (`MMB_NET_RX_CAP`) already holds the socket data.

### Framing

After `ESC`, byte `P` enters DCS. Bytes up to the final character
(`0x40..0x7E`) are the parameter string (digits and `;`). The final
character selects the command:

- `q` — sixel. Enter the pixel decoder with P1, P2, P3.
- anything else (`$ q` DECRQSS, ReGIS `p`, and the rest) — discard bytes
  until `ST`. Do not print them.

`ST` is `ESC \` (0x1B 0x5C). Inside an open DCS, a lone `ESC` that is not
the start of `ST` aborts the image and that `ESC` is fed back into
`ansi_feed`, so a missing terminator cannot eat the rest of the session.
`CAN` (0x18) and `SUB` (0x1A) abort as well. `CR`, `LF`, `TAB` and `SP`
inside the DCS are ignored. libsixel wraps the payload with CRLF; those
must not become `pane_newline`.

8-bit `ST` (0x9C) can be accepted inside a DCS as a convenience. 8-bit
`DCS` (0x90) stays unrecognised in the first cut. Tools we care about
send 7-bit.

Do not reuse the 2 s OSC watchdog. A full-screen image over a BBS link
is silent for longer than that while bytes are still arriving. Abort a
DCS when no byte has arrived for about 30 s, or when the body exceeds a
cap (4 MB of DCS text is enough for a pane-sized image with repeats). On
abort, return to ground and do not print the tail.

The ZMODEM detector runs on the raw stream before `ansi_feed`. Sixel data
lives in `0x3F..0x7E` and should not contain `CAN`, but `*` `*` `CAN`
inside a DCS would steal the session. While a DCS is open, that scan
stays off.

Telnet IAC is stripped before `incoming_byte`. A well-formed sixel has no
`0xFF`. No special case is required.

### Parameters before `q`

`DCS P1 ; P2 ; P3 q` (xterm writes them `Pa`, `Pb`, `Ph`). Omitted
parameters are 0.

| P1 | DEC pixel aspect |
| --- | --- |
| 0, 1, 5, 6 | 2:1 |
| 2 | 5:1 |
| 3, 4 | 3:1 |
| 7, 8, 9 | 1:1 |

HDMI pixels are square, and `img2sixel` / `chafa` describe size in real
pixels (often with raster attributes, below). Honouring P1 would stretch
those images. Ignore P1 and draw 1:1.

P2 is the background select, from the VT340 and from xterm:

- 0 or 2 (default): a sixel bit that is 0 is painted with the background
  colour captured at the start of the image.
- 1: a 0 bit is left unchanged (transparent).

P3 is the horizontal grid size. Ignore it. The raster-attribute width
overrides it on every encoder that sets a width.

The background colour is `term_paper()` at the moment `q` is seen: the
SGR paper, default black `0x000000`, not the theme colour. SGR cannot
occur inside the DCS, so one sample is enough. Store it already packed
through `mmb_pix_store`.

### Picture data

A sixel character is a byte `0x3F..0x7E`. The pixel value is `byte - 0x3F`
(0..63). Bit 0 is the top pixel, bit 5 the bottom. One character paints
six vertical pixels at the current position in the active register, then
advances one pixel to the right.

| Byte | Effect |
| --- | --- |
| `!` Pn | Repeat the next sixel character Pn times. Pn is decimal. One character follows, even if it is `?`. |
| `#` Pc | Select register Pc. |
| `#` Pc `;` Pu `;` Px `;` Py `;` Pz | Define register Pc and select it. |
| `"` Pan `;` Pad `;` Ph `;` Pv | Raster attributes. |
| `$` | Return to the left edge of this image, same six-pixel band. |
| `-` | Next band: six pixels down, and back to the left edge. |
| other C0, and printables outside commands and sixel data | Ignore. |

`$` is the left edge of the image (the cursor column where it started),
not column 0 of the pane, when the image did not start at column 0.

`"` sets the raster:

- `Ph`, `Pv` — width and height in pixels. Clip all drawing to that box.
  Cursor advance uses this box when it is present, and the pixels actually
  touched when it is not.
- When P2 is 0 or 2 and `Ph`/`Pv` are present, fill that rectangle with
  the captured background before any sixel. Pixels never addressed then
  match the background. When `Ph`/`Pv` are absent, only pixels a sixel
  character actually covers are written; a 0 bit still paints background
  if P2 is 0 or 2.
- `Pan`/`Pad` are the pixel aspect numerator and denominator. Ignore them
  in the first cut (square pixels). `"1;1;width;height` is what current
  encoders send.

Repeat count saturates at the remaining pixels in the raster (or the pane
width). `!0` and a missing character are no-ops. A parameter that does
not fit in a 32-bit unsigned value saturates. That stops `!999999999`
from spinning.

Right edge: clip. Sixels do not wrap to the next line.

### Palette

256 registers. That covers `img2sixel -p 256` and chafa's usual sixel
quantiser. XTSMGRAPHICS reports 256 so a tool that asks will not emit
more. A register index above 255 is clipped to 255.

Each image starts from the VT340 default palette (the 16 colours
libsixel ships as the VT340 colour table). Registers 16..255 start black.
Resetting per image matches xterm's private-register default without
implementing mode 1070. One image's definitions must not leak into the
next.

| Reg | RGB | Reg | RGB |
| --- | --- | --- | --- |
| 0 | `#000000` | 8 | `#444444` |
| 1 | `#3333CC` | 9 | `#000099` |
| 2 | `#CC2121` | 10 | `#990000` |
| 3 | `#33CC33` | 11 | `#009900` |
| 4 | `#CC33CC` | 12 | `#990099` |
| 5 | `#33CCCC` | 13 | `#009999` |
| 6 | `#CCCC33` | 14 | `#999900` |
| 7 | `#777777` | 15 | `#CCCCCC` |

Colour introducer `#` Pc `;` Pu `;` Px `;` Py `;` Pz:

- Pu = 2: RGB. Each component is a percentage 0..100. `value * 255 / 100`.
  `#0;2;100;0;0` is pure red. Values above 100 clamp to 100. Encoders
  that send 0..255 are out of spec; do not guess. `img2sixel` and chafa
  send 0..100.
- Pu = 1: DEC HLS. Hue 0..360, lightness and saturation 0..100. Hue 0 is
  blue, not red (the DEC convention shared by xterm and libsixel).
  Saturation 0 is grey at `L * 255 / 100`. Use libsixel's `hls_to_rgb`
  as the oracle and lock it with unit-test vectors: `(H,L,S) = (0,50,100)`
  is blue, `(120,50,100)` is red, `(240,50,100)` is green, `(0,50,0)` is
  grey. Convert to a `uint16_t` once, through the caller's pack function,
  and store that in the register.

The decoder's pack hook is `mmb_pix_store` inside TERM. The host test
supplies its own pack so it does not need Circle. Pixel writes then store
the register's `uint16_t` directly. No per-pixel RGB888 conversion.

## Images and the text grid

Page 2 is rebuilt from cells. A sixel painted only into page 2 disappears
on the next `term_draw_row` of those cells, and it does not move when
`pane_scroll_up` shifts the cell arrays. The image has to live beside
the grid.

### Plane

One native-pixel plane per TERM console, allocated on the first sixel and
freed in `term_exit` with the scrollback ring:

```
bytes = pane_cols * 8 * pane_rows * cell_h * 2
```

| View | Plane |
| --- | --- |
| Boxed MODE 14, 80×32 of 8×16 | 640×512×2 = 640 KB |
| 80×25 MODE 2 | 640×400×2 = 512 KB |
| Full 1920×1080, 240×66 of 8×16 | about 4 MB |

A coverage flag per cell (80×256 bytes, or a bitset) records which cells
the plane owns. `term_draw_row` branches:

- coverage set: copy that cell's 8×`cell_h` pixels from the plane into
  page 2. Adjacent graphics cells on a scanline are one `memcpy`.
- coverage clear: the existing glyph path.

The glyph shadow key must not suppress a graphics cell. When coverage
is cleared, zero that cell's shadow so the next glyph paints.

Menu and dialog drawing still go straight to page 2. `mark_dirty_full`
on the way out repaints from the plane, so the image returns.

If the allocation fails, discard the DCS until `ST` and leave the pane
alone. Do not fall through to `pane_put`.

Only the visible pane is stored. Four consoles can each hold a TERM
session, so the plane is per console and only while that session is
inside TERM.

### Placement and the cursor

Default mode is sixel scrolling on (see DECSDM below).

- Origin is the top-left pixel of the cursor cell:
  `(cur_col * 8, cur_row * cell_h)` in pane coordinates.
- Bands are 6 pixels tall. Cell height is 14 or 16, so an image does not
  land on cell boundaries. Coverage is every cell the pixels touch.
- After `ST`, the cursor goes to column 0 of the first cell row below the
  image. The row count is `ceil(height / cell_h)`. A partial last band
  still consumes that cell row.
- Pixels past the right edge of the pane are clipped.
- If the image crosses the bottom margin, scroll the scrolling region the
  same way a newline does, including the plane (see below), and keep
  writing. Cap one image at one pane of height. Further bands are clipped.
  With scrolling on, the visible part is the bottom of the image and the
  cursor sits on the row under it. That is what a tall `img2sixel` photo
  needs. An unbounded `Pv` cannot walk the scrollback forever.
- Mode 8452 (leave the cursor to the right of the image) is out of scope.
  `lsix` wants the cursor underneath.

With scrolling off (DECSDM set), the origin is the top-left of the pane,
the cursor does not move, the image does not scroll, and the part below
the pane is clipped.

### Scrolling, overwrite, clear

Every operation that moves or erases cells moves or erases the same
pixels. One helper pair is enough: copy a pixel rectangle in the plane,
and clear coverage on a cell rectangle. Call sites that already loop over
cells:

| Operation | Plane |
| --- | --- |
| `pane_scroll_up`, SU, SD | Move the pixel strip by `n * cell_h`. The row pushed to scrollback drops its pixels (first cut). |
| IL, DL | Same vertical move, bounded by DECSTBM, matching the cell loops. |
| ICH, DCH | Horizontal move by `n * 8` pixels inside the touched rows. |
| `pane_put`, REP | Clear coverage on the cell being written. The glyph path draws it. Stale plane bytes are never read once coverage is clear. |
| EL, ED, ECH, BCE blanks | Clear coverage. The space-with-paper glyph is the cleared look, same as text. |
| RIS (`ESC c`), form feed, ED 2/3 | Clear the whole plane's coverage. |

Newline scroll of a row that contains sixel pixels has to `memmove` the
plane once per scrolled row, by `cell_h` scanlines. That matches the
existing cell scroll. The present stays coalesced: the interpreter still
dirties a band and presents once per poll.

Sub-region DECSTBM scrolls only the pixel rows inside the region. Pixels
above `scroll_top` stay.

### Scrollback, resize, limits

First cut: a row that leaves the pane keeps its characters and colours in
the scrollback ring and drops its pixels. Page Up shows the underlying
cells, which are spaces where the image replaced them. `lsix` only needs
the image to scroll while it is on screen. Keeping strips in the ring is
a later ticket: a separate byte budget (1 MB), oldest strip discarded,
Page Up blits from that store. Do not multiply the 576 KB text ring by
a full-width RGB565 line. One boxed line is `640 * 16 * 2 = 20 KB`; 200
of them are 4 MB, and a 1920-wide line is about 60 KB.

`term_cycle_view` changes MODE and the pane size. Free the plane, clear
coverage, and let the next image allocate the new size. Text cells stay,
as they do today. Rescaling a live image across a view switch is not
worth it.

Limits to enforce in the decoder and the session:

- Plane size as above, one allocation, freed with the session.
- 256 registers, `uint16_t` each, inside the decoder (512 bytes).
- Repeat clamped to the remaining width.
- DCS body cap 4 MB, idle abort ~30 s, `CAN` / `SUB` / bare `ESC`.
- One image scrolls at most one pane, then clips.
- Allocation failure discards until `ST`.

## Queries and DECSDM

Tools decide whether to send sixel, and at what size, from the replies.
`ansi_reply` currently transmits only when `T.tcp` is set, so demo mode
and `TERM REPLAY` do not answer. The query tests need a TCP session (the
existing QEMU Ethernet harness) unless reply logging is extended on
purpose. Replies are already logged as `T` lines when `OPTION TERM LOG`
is on.

### DA1

`CSI c` and `CSI 0 c` today return `CSI ? 1 ; 2 c`. libsixel and chafa
look for a parameter equal to 4 (the VT220 feature flag for sixel
graphics). A VT100-style body does not officially carry those flags, but
`CSI ? 1 ; 2 ; 4 c` keeps the reply the BBS autodetection path already
sees (`?1;2`) and adds a `;4` parameter those tools accept.

Do not switch the identity to `CSI ? 62 ; 4 ; 22 c` in the same change.
The help text documents the current DA reply as the BBS probe. A VT220
service class can be a follow-up if a real tool rejects `?1;2;4c`.

Secondary DA (`CSI > c`) stays unimplemented.

### XTWINOPS

Implement only the report queries. Window move, resize, iconify and
fullscreen (`CSI Ps t` for Ps 1..10) stay ignored. TERM is not a window.

Report the pane, not the letterbox and not the whole HDMI frame.
`img2sixel` sizes the picture from these numbers.

| Query | Reply | Value |
| --- | --- | --- |
| `CSI 14 t` | `CSI 4 ; height ; width t` | `pane_rows * cell_h`, `pane_cols * 8` |
| `CSI 16 t` | `CSI 6 ; height ; width t` | `cell_h`, `8` |
| `CSI 18 t` | `CSI 8 ; height ; width t` | `pane_rows`, `pane_cols` |

`CSI 14 ; 2 t` (window pixels including decoration) can return the same
pane size. There is no decoration. `CSI 11 t` (window state) can return
`CSI 1 t`. Anything else in this family stays ignored.

The parser already has eight parameter slots. These queries use one or
two.

### XTSMGRAPHICS

`CSI ? Pi ; Pa ; Pv S`. Private CSI with final `S`. `ansi_exec_priv`
has to grow past mode 25; today a private final other than `h`/`l` is
dropped.

| Pi | Pa | Reply on success |
| --- | --- | --- |
| 1 colour registers | 1 read, 4 read max | `CSI ? 1 ; 0 ; 256 S` |
| 2 sixel geometry | 1 read, 4 read max | `CSI ? 2 ; 0 ; width ; height S` with the pane pixel size |
| 1 or 2 | 2 reset | success, same current value |
| 1 or 2 | 3 set | `CSI ? Pi ; 3 ; 0 S` failure. The plane tracks the pane. |
| 3 ReGIS, or unknown Pi | any | `CSI ? Pi ; 1 ; 0 S` or status 3. No ReGIS. |

Status codes, matching xterm: 0 success, 1 bad Pi, 2 bad Pa, 3 failure.
Geometry reported here is the same pair as `CSI 14 t`.

### DECSDM

Private mode 80. The VT340 manual and the VT340 hardware disagree, and
xterm changed sides in patch 369. What to implement:

- Session start: scrolling **on**. The image is placed at the cursor, the
  cursor advances to the row below, and the image scrolls with text.
  `img2sixel`, `chafa` and `lsix` do not send DECSDM. They assume this.
  xterm's `sixelScrolling` resource also defaults to on after patch 374.
- `CSI ? 80 h` (set): scrolling **off**. Image at the pane origin, cursor
  stays, no scroll, clip at the bottom. This is xterm 369 and later, the
  VT382 manuals, and the VT340's actual behaviour (display mode).
- `CSI ? 80 l` (reset): scrolling **on** again.

The VT340 programmer's manual says the opposite (set enables scrolling).
xterm followed that manual until patch 369, and some emulators still do.
Do not "correct" the polarity back to that manual. `lsix` against a
terminal whose power-on state is non-scrolling draws every thumbnail on
top of the others at the upper left.

Out of scope with DECSDM: mode 1070 (private colour registers; per-image
palette reset covers it) and mode 8452 (cursor to the right of the image).

## Performance on the Pi

The expensive part of a sixel is the HDMI upload, which TERM already pays
for text. The decode should not add a second full-frame copy.

- Pack each palette register once, when it is defined or when the default
  palette is loaded. The hot loop stores `uint16_t`.
- Write those pixels into the plane, not into an RGB888 intermediate and
  not glyph-by-glyph through `mmb_gfx_plot`.
- Track the pixel bounding box of the image. Present that rectangle, not
  the full dirty-row band the text path uses. `term_present_async_rect`
  already takes `x, y, w, h`. A box narrower than the page stride is
  bounce-copied and then DMA'd; a full-width band can DMA from page 2
  directly. Either is cheaper than uploading 960×540 for a thumbnail.
- `term_draw_row` copies plane → page 2 only for covered cells, in
  scanline `memcpy`s. Uncovered cells keep the shadow-key skip.
- Scroll is one `memmove` of the plane per scrolled text row, plus the
  cell move that already exists. A burst still presents once per poll.
- Yield every few kilobytes of DCS body (`TM_INTERP_YIELD` is 256 bytes
  today) so the mixer and the TCP window stay alive. Do not present on
  each yield. Background drain (`s_term_bg`) updates the plane and leaves
  `need_draw` set, the same rule as text.
- Do not decode inside the DMA source. `term_guard_present` already
  waits until page 2's in-flight DMA finishes before a repaint. The plane
  is not the DMA source, so decode can fill the plane while the previous
  frame is scanning out. The copy into page 2 happens after the guard,
  inside `term_draw`.

A boxed thumbnail (a few hundred pixels on a side) should be one palette
pack, one plane fill, one cell-band copy, and one DMA of that rectangle.

## Test plan

### Host unit tests of the decoder

Compile `sixel.c` with a tiny driver, the same pattern as
`tests/test_paint_damage.py`. The driver supplies a `uint16_t` buffer and
a deterministic pack (the HDMI `rgb_to_raw` shifts are fine here, because
the test owns both sides). No QEMU.

Cases, as raw sixel strings:

1. One red column. `ESC P q #0;2;100;0;0 ~ ESC \` paints six red pixels,
   bit 0 at the top. The `uint16_t` matches the pack of `0xFF0000`.
2. Repeat. `!6?` (or `!6~` with a defined register) paints six identical
   columns. `!0` and a truncated repeat paint nothing.
3. `$` returns to the left edge and overwrites the band. `-` advances
   exactly six pixels and also returns to the left edge.
4. P2 = 0 fills 0-bits with the captured background, so a later register
   does not show through a 0 bit. P2 = 1 leaves those pixels untouched.
5. `"1;1;4;6` clips a longer band to 4×6. Cursor-advance height reported
   by the decoder is 6, not the unclipped data.
6. HLS vectors above, plus saturation 0.
7. Default register 2 is `#CC2121` before any `#` definition. Defining
   register 0 does not change register 1.
8. CRLF inside the payload does not move the cursor. Bytes outside
   commands and `0x3F..0x7E` are ignored.
9. `ESC P $ q` … `ST` (not sixel) yields an empty image and consumes the
   body. `CAN` mid-image stops the decoder. A body larger than the cap
   stops it.
10. Register 300 clips to 255. A huge repeat does not walk off the buffer.

The decoder test does not need the grid. A second, still host-side, test
can feed the same bytes through a stub of the coverage helper if that
helper is a pure function (cell flags in, pixel rectangle out).

### QEMU

These go through the existing TERM harness (`MMBasicConsole`,
`capture_png`), not a new runner.

- `TERM REPLAY` of a `.termlog` whose `R` line is case 1. The PNG pixel
  at the cursor cell's top-left is red within RGB565 quantisation
  (5-bit red, so `0xFF` lands on the top red step). The cells under the
  image are not the characters `q` and `#`.
- A second replay: text, then the image, then `OK`. `OK` is on the row
  below the image, column 0. Scrolling the pane (enough newlines, or an
  image taller than the remaining rows) moves the red pixels up with the
  text.
- Overwrite: after the image, `CUP` back and print `X`. That cell is the
  glyph, the neighbouring sixel pixels remain.
- `ED 2` clears the red pixels.
- View cycle (F10, View) after an image does not hang and does not leak
  the plane across the MODE change. The image may disappear; the pane
  must still repaint text.
- Page Up after the image shows the text row, not a crash and not a
  torn framebuffer. Pixels are absent in this cut.
- DA, `CSI 14 t`, `CSI 16 t`, `CSI 18 t`, and `CSI ? 1 ; 1 ; 0 S` /
  `CSI ? 2 ; 1 ; 0 S` on a TCP session. Assert the bytes (`;4` in the DA
  reply, pane pixel size, cell 16×8 or 14×8, 256 registers). File replay
  will not show these until `ansi_reply` runs without `T.tcp`.
- `CSI ? 80 h` then an image: cursor unchanged, image at the pane origin.
  `CSI ? 80 l` restores cursor advance.

Keep the suite on replay plus one TCP test. Do not call live network
tools from the default pytest run.

### img2sixel, chafa, lsix

Opt-in, same spirit as `MMCORE_LIVE_NET`. QEMU SLIRP to the host is not
reliable enough for the parallel suite.

Capture one `.termlog` from each tool against a known PNG (a solid red
square, a 16-colour pattern, and a photo that needs more than 16
registers) and replay those in QEMU. That locks the wire format the tools
actually emit, including their raster attributes and whether they send
DA / `CSI 14 t` first.

On hardware, or with `MMCORE_LIVE_NET=1`:

- `img2sixel -p 16` and `-p 256` of the same PNG. The 256-colour run must
  not fall back to printing the DCS.
- `chafa --format sixel` of the same PNG.
- `lsix` of a directory of a few small PNGs. Thumbnails stack vertically,
  each filename stays with its image, and a following prompt is below the
  last image rather than on top of it.

Check the HDMI PNG, not the serial dump. Live TCP does not dump the pane
to serial.

## Ticket breakdown

Not filed. Each ticket is independently mergeable. Later tickets depend
on earlier ones in the same phase only where the acceptance test says so.

### 1. DCS framing

Recognise `ESC P` … `ST` in `ansi_feed`. Non-`q` DCS is discarded. `q`
calls into the decoder (a stub that ignores pixels is enough for this
ticket). `CAN`, `SUB`, and a bare `ESC` leave DCS. CRLF inside is not a
newline. ZMODEM scan is suppressed while DCS is open. Idle and byte-cap
aborts are in place.

Acceptance: a replay of case 1 from the test plan does not print
`q`, `#` or `~`. Text after `ST` prints at the cursor the image did not
move yet. A DECRQSS-shaped DCS (`ESC P $ q` … `ST`) prints nothing.
A DCS with no `ST` returns to text after the cap or the idle abort.

### 2. Decoder, palette, host tests

`sixel.c`: P1 ignored, P2 background, P3 ignored, raster `"`, `#` select
and define (HLS and RGB), `!`, `$`, `-`, sixel bytes, 256 registers,
VT340 defaults, pack callback, clip, repeat saturation.

Acceptance: the host tests in the test plan pass. No QEMU. No change to
pixels on screen yet beyond the stub from ticket 1.

### 3. Plane, cursor, scroll, dirty rectangle

Allocate the plane, set coverage, blit covered cells in `term_draw_row`,
place the image at the cursor, move the cursor below it, scroll the plane
with the scrolling region, clip at one pane, present the image rectangle.

Acceptance: the red-column replay shows a red pixel at the cursor cell
origin in the HDMI PNG. A line printed after the image is on the next
row. An image that runs past the bottom margin scrolls, and the red
pixels move up with the text. A thumbnail does not present the whole
960×540 frame (assert the present rectangle, or the damage recorded by
the platform hook, is the image box). Background drain does not paint,
and the image appears when the console is shown again.

### 4. Overwrite and erase

`pane_put`, REP, EL/ED/ECH, IL/DL, ICH/DCH, SU/SD, RIS and form feed
update coverage and the plane.

Acceptance: the overwrite, `ED 2`, and DECSTBM cases in the test plan.
Inserting a line above an image moves the image down by one cell row.
Deleting the image's cells removes its pixels. A character written on
top of the image is a glyph on the next repaint, including after the
menu is opened and closed.

### 5. DA1, XTWINOPS, XTSMGRAPHICS, DECSDM

Replies as specified above. `CSI ? 80 h/l` toggles scrolling. Report
queries only; window-management `CSI t` ops stay ignored.

Acceptance: the TCP query test. DECSDM off pins the image to the pane
origin and leaves the cursor. DECSDM on (the session default, and after
`CSI ? 80 l`) matches ticket 3. A BBS-style read of the DA reply still
sees `?1;2`.

### 6. Resize, allocation failure, scrollback policy

Free the plane on view change and on `term_exit`. Failed allocation
discards the DCS. Page Up does not look at freed pixels.

Acceptance: cycle Full / Boxed / 80×25 after an image and keep typing.
Leave TERM and enter it again (plane allocated once, freed once; no
leak across the four consoles). Page Up shows text and does not fault.
A forced allocation failure (host test of the discard path) does not
print the payload.

### 7. Tool captures, then live img2sixel / chafa / lsix

Add three replay fixtures captured from those tools. Document the
opt-in live command in the test, gated like `MMCORE_LIVE_NET`.

Acceptance: the fixtures pass in the normal suite. The live run is
skipped unless the env var is set. `lsix` stacks images with the text
between them.

### Later, not in the first pass

- Pixel scrollback under a 1 MB cap, Page Up blits the strip.
- `Pan`/`Pad` and P1 aspect, if a real tool sends non-square pixels and
  the square result is wrong.
- Mode 8452 and mode 1070.
- 8-bit C1 `DCS`.
- VT220 DA identity (`?62;4;22c`) if a tool rejects `?1;2;4c`.
- ReGIS, kitty graphics, iTerm2 inline images.

## Out of scope

ReGIS, sixel macros (`DECDMAC`), DRCS, and any graphics protocol other
than sixel. Changing the CP437 cell grid into a Unicode terminal.
Uploading sixel from the keyboard. Animating a multi-frame sixel dump
(a later frame can replace pixels; no timing).
