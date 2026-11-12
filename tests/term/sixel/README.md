# Sixel tool fixtures

Real encoder output replayed by `tests/test_term_sixel.py`, so the QEMU suite
locks the wire format that `img2sixel`, chafa and lsix actually emit.  The
source PNGs are included for reproducibility.

Captured on 2026-11-04 with:

- img2sixel 1.8.7-r2 (libsixel)
- Chafa 1.18.3
- lsix 1.9.1

Commands (from this directory):

```sh
img2sixel -p 16 -w 16 -h 16 red.png > red-img2sixel-p16.sixel
img2sixel -p 256 -w 32 -h 32 photo.png > photo-img2sixel-p256.sixel
chafa --format=sixels --colors=16 --size=8x8 --view-size=8x8 \
    pattern.png > pattern-chafa-16.sixel
LSIX_FORCE_SIXEL_SUPPORT=1 lsix red.png pattern.png > two-lsix.sixel \
    </dev/null 2>/dev/null
```

`lsix` montages the two images and their labels into one 756x140 sixel, which
also exercises right-edge clipping in the 640-pixel boxed pane.

The fixtures are replay-only.  Live runs against the installed tools are
opt-in with `MMCORE_LIVE_NET=1`, matching the other live-network tests.
