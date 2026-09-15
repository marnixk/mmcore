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

## Still software

Page-to-page `PAGE COPY`, transparent blit, logic ops, and page-1 overlay
composite (expand + blend in RGB888, store native into `present_scratch`) remain
CPU work. Follow-ups: dedicated DMA for large copies, virtual-offset
`PAGE DISPLAY`, dirty-rect / async present.
