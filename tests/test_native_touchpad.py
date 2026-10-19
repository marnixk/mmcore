"""#1039: an evdev touchpad/clickpad reaches SDL as a *touch* device.

SDL's raw evdev backend treats a touchpad like a touchscreen and calls
``SDL_SendTouch(..., window=NULL, ...)``; SDL's built-in touch-to-mouse
synthesis is gated on a non-NULL window, so it never emits ``SDL_MOUSEMOTION``.
The native input must therefore turn ``SDL_FINGER*`` motion into pointer
movement itself. These tests drive the real software pointer through the
``native/mmcore`` SDL harness.

Guarded and skipped without a host C toolchain / SDL2, like
``test_paint_native.py`` and ``test_paint_file.py``.
"""
import os
import shutil

import pytest

from native_harness import (
    NativeSession,
    SDL_BIN,
    build_native,
    sdl_backend_available,
)

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def native_mmcore():
    build_native()
    if not sdl_backend_available():
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    return SDL_BIN


def _read_pointer(path: str) -> tuple:
    with open(path) as fh:
        x, y = fh.read().split()
    return int(x), int(y)


def test_touchpad_finger_motion_moves_pointer(native_mmcore, tmp_path):
    s = NativeSession(tmp_path)
    before = s.pointer("before.txt")
    s.finger(0.25, 0.25)
    after = s.pointer("after.txt")
    s.quit()
    s.run()

    bx, by = _read_pointer(before)
    ax, ay = _read_pointer(after)
    assert (ax, ay) != (bx, by), (bx, by, ax, ay)
    assert ax > bx and ay > by


def test_touchpad_subpixel_drag_still_accumulates(native_mmcore, tmp_path):
    """A slow drag sends many tiny deltas; the sub-pixel remainder must not be
    dropped, or the pointer would never move."""
    s = NativeSession(tmp_path)
    before = s.pointer("before.txt")
    for _ in range(40):
        s.finger(0.002, 0.002)
    after = s.pointer("after.txt")
    s.quit()
    s.run()

    bx, by = _read_pointer(before)
    ax, ay = _read_pointer(after)
    assert ax > bx and ay > by
