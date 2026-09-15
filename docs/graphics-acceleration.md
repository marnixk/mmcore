# Graphics acceleration (Circle / Raspberry Pi)

MMBasic graphics pages and the soft framebuffer store **HDMI-native** pixels
(Circle `COLOR16` / `DEPTH=16`: 5-5-5). RGB888 appears only at the MMBasic API
boundary (`RGB()`, `PIXEL`, `PSET`, sprites, blit scratch); helpers expand on
read and quantize/convert on write.

Visible updates prefer `plat_present_native` → `CBcmFrameBuffer::SetArea` with
no RGB888→native convert loop. `plat_present_rgb` remains for callers that still
hand RGB888 (and as a fallback).

## Screen DMA present (hardware)

Circle enables `SCREEN_DMA_BURST_LENGTH` by default. `SetArea` then copies the
rectangle into the HDMI framebuffer with `CDMAChannel::SetupMemCopy2D`.

`scripts/build.sh` passes `configure --qemu` for the harness image. That adds
`NO_SCREEN_DMA_BURST_LENGTH`, so QEMU keeps the CPU memcpy `SetArea` path
(framebuffer DMA is unreliable under QEMU).

Hardware release builds use `QEMU=0` (`scripts/package-release.sh` →
`build_hardware`). Those kernels get DMA present on Pi 2–4 / 400 without extra
flags.

Present / TUI bounce buffers in `console/platform.cpp` are heap-allocated
(cache-line aligned by Circle) with sizes rounded via `CACHE_ALIGN_SIZE` so DMA
cache maintenance does not touch neighbouring heap metadata. Native present
uses the page buffer directly when `stride == w`; otherwise it packs rows into
the bounce buffer.

## Page / blit memory DMA (hardware)

`console/platform.cpp` owns a second `CDMAChannel` (`DMA_CHANNEL_NORMAL`, or
`DMA_CHANNEL_EXTENDED` when `RASPPI >= 4`) and exposes:

- `plat->dma_copy(dst, src, nbytes)` → `SetupMemCopy` (cached buffers)
- `plat->dma_copy2d(dst, src, block_len, block_count, block_stride)` →
  `SetupMemCopy2D` (packed source rows into a pitched destination; destination
  cache is cleaned/invalidated by the platform wrapper)

Both return `1` if DMA ran, or `0` so the caller falls back to `memcpy`.

Under QEMU (`NO_SCREEN_DMA_BURST_LENGTH`) both hooks always return `0` — memory
DMA may work in some QEMU builds, but the safe path matches screen DMA and
keeps the harness on memcpy.

Call sites:

- Opaque `PAGE COPY` (`mmb_gfx_copy_page` without `,B`): if the page is at least
  4096 bytes, try `dma_copy`, else `memcpy`. Transparent `,B` copies stay CPU.
- Opaque rectangular `BLIT` (`ori == 0`, no skip-black / flip): native
  `uint16_t` row or 2D DMA when non-overlapping; transparent / logic / rotated
  blits stay on the RGB888 snap path.

Page buffers come from the Circle heap (already cache-line aligned).

## Dirty rectangles and async present

Drawing that touches the display page (or page-1 overlay) expands a dirty AABB
(`mmb_gfx_dirty_add`). Opaque `PAGE COPY` / `BLIT` onto the visible page mark
the copied region dirty. `mmb_gfx_present()` prefers `present_rect` of that AABB
when dirty is set; otherwise it presents the full frame (MODE / `PAGE DISPLAY` /
CLS of the display clear dirty first so the present stays full-screen).

On hardware (`SCREEN_DMA_BURST_LENGTH`), BASIC `plat_present_native` /
`plat_present_rgb` use synchronous `SetArea` (wait for DMA completion before
return). TERM uses `plat->term_present_async` / `plat->term_present_drain`:
one async blit in flight, pending damage coalesced into a Y-range (pixel rows;
`0xffffffff` = empty), double bounce buffers so CPU copies never touch memory
the in-flight DMA reads, and drain on TERM exit or mode change. Under QEMU
(`NO_SCREEN_DMA_BURST_LENGTH`) TERM presents are still synchronous; drain is a
no-op.

## PAGE DISPLAY virtual-offset flip (Pi ≤ 4 hardware)

Soft graphics pages stay heap-allocated. On Pi ≤ 4 hardware builds, Circle’s
`CScreenDevice` creates a double-buffered HDMI framebuffer (`bDoubleBuffered`,
virtual height = 2× physical) via `patches/circle-fb-doublebuf.patch`.

`console/platform.cpp` tracks which half is scanned out (`s_fb_front`). A
`SetDrawOffsetY` on `CBcmFrameBuffer` remaps `SetPixel` / `SetArea` so TUI text,
dirty-rect presents, and immediate `set_pixel` always write the *visible* half.

Full-frame present (dirty cleared — `PAGE DISPLAY`, MODE, CLS of the display)
uses the flip path:

1. `SetDrawOffsetY(back * height)` and synchronous `SetArea` into the hidden half
2. `WaitForVerticalSync`
3. `SetVirtualOffset(0, back * height)` and swap `s_fb_front`

Under QEMU (`NO_SCREEN_DMA_BURST_LENGTH`) the Screen device stays single-buffered
and presents stay synchronous without virt-offset (QEMU virt-offset is
unreliable). On Pi 5 (`RASPPI > 4`) Circle has no legacy virt-offset double-buffer
path (same limit as `C2DGraphics`); presents use the single-buffer SetArea path.

## Still software

Transparent blit, logic ops, and page-1 overlay composite (expand + blend in
RGB888, store native into `present_scratch`) remain CPU work.
