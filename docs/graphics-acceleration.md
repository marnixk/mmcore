# Graphics acceleration (Circle / Raspberry Pi)

MMBasic pages are software buffers. Visible updates go through
`plat_present_rgb` → `CBcmFrameBuffer::SetArea`.

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
cache maintenance does not touch neighbouring heap metadata.

## Still software

Page-to-page `PAGE COPY`, transparent blit, logic ops, and RGB888→native
convert before present remain CPU work. See follow-ups: native-depth pages,
dedicated DMA for large copies, virtual-offset `PAGE DISPLAY`, dirty-rect /
async present.
