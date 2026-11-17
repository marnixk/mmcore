"""HUB screens (#1526): key handling and layout in the native SDL build.

Drives ``native/mmcore`` through ``native_harness.NativeSession`` against the
hermetic fixture server from ``test_hub.py``. Covers tab switching, opening the
detail page, Esc returning to the same tab/selection, and remove with N/Y.

The pure layout maths live in ``test_hub_layout.py``; these tests need the SDL
backend and are skipped without it.
"""

import os
import shutil
import time

import pytest

from native_harness import NativeSession, Ppm, SDL_BIN, build_native, \
    sdl_backend_available
from test_hub import APPS, TINY_PNG, HubServer, app_entry, index_doc, \
    make_app, preinstall, set_url

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


@pytest.fixture()
def hub():
    s = HubServer(None)
    s.add_apps()
    try:
        yield s
    finally:
        s.close()


def _session(tmp_path, hub, mode="9,16", preinstalled=("jetpack",)):
    root = os.path.join(str(tmp_path), "root")
    os.makedirs(os.path.join(root, "C", "APPS"), exist_ok=True)
    for app_id in preinstalled:
        preinstall(root, app_id, make_app(app_id, app_id.title(), "1.0"))
    work = os.path.join(str(tmp_path), "work")
    s = NativeSession(work, env={
        "MMB_DRIVE_ROOT": root,
        "MMCORE_RELEASE_VERSION": "0.312.0",
    })
    s.feed('OPTION PATH "C:/APPS/"')
    s.feed(set_url(hub.base))
    s.feed("MODE %s" % mode)
    s.feed("HUB")
    s.wait_ms(2000)
    s.start()
    return s, root


def _same(a: Ppm, b: Ppm) -> bool:
    return a.data == b.data


def _wait_file(path, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.isfile(path):
            return True
        time.sleep(0.02)
    return False


def test_listing_fetches_hero(native_mmcore, hub, tmp_path):
    entry = app_entry("jetpack", category="GAMES")
    entry["hero"] = "info/jetpack/HERO.PNG"
    hub.add_index("/index.json", index_doc(apps=[entry]))
    hub.routes["/info/jetpack/HERO.PNG"] = (200, TINY_PNG, "image/png")
    s, _ = _session(tmp_path, hub, preinstalled=())
    s.quit()
    out = s.run()
    assert "?SYNTAX" not in out, out
    assert "/info/jetpack/HERO.PNG" in hub.requests, hub.requests


def test_tabs_left_right(native_mmcore, hub, tmp_path):
    s, _ = _session(tmp_path, hub)
    listing = s.snapshot("listing.ppm")
    s.key("right")
    s.wait_ms(180)
    other = s.snapshot("other.ppm")
    s.key("left")
    s.wait_ms(180)
    back = s.snapshot("back.ppm")
    s.quit()
    out = s.run()
    assert "?SYNTAX" not in out, out
    assert not _same(listing, other), "Right did not change the tab"
    assert _same(listing, back), "Left did not restore the ALL tab"


def test_enter_detail_esc_keeps_selection(native_mmcore, hub, tmp_path):
    s, _ = _session(tmp_path, hub)
    first = s.snapshot("first.ppm")
    s.key("down")
    s.wait_ms(180)
    second = s.snapshot("second.ppm")
    s.key("enter")
    s.wait_ms(250)
    detail = s.snapshot("detail.ppm")
    s.key("esc")
    s.wait_ms(180)
    back = s.snapshot("back.ppm")
    s.quit()
    s.run()
    assert not _same(first, second), "Down did not move the selection"
    assert not _same(second, detail), "Enter did not open the detail page"
    assert _same(second, back), "Esc did not return to the same selection"


def test_remove_no_keeps_and_yes_deletes(native_mmcore, hub, tmp_path):
    s, root = _session(tmp_path, hub)
    path = os.path.join(root, "C", "APPS", "jetpack.APP")
    assert os.path.isfile(path)
    listing = s.snapshot("listing.ppm")

    s.key("delete")
    s.wait_ms(150)
    modal = s.snapshot("modal.ppm")
    s.text("n")
    s.wait_ms(150)
    kept = s.snapshot("kept.ppm")
    assert not _same(listing, modal), "Del did not open the remove modal"
    assert _same(listing, kept), "N did not return to the listing"
    assert os.path.isfile(path), "N removed the file"

    s.key("delete")
    s.wait_ms(150)
    s.text("y")
    s.wait_ms(400)
    s.quit()
    s.run()
    assert not os.path.exists(path), "Y did not remove the file"


def test_remove_on_not_installed_does_nothing(native_mmcore, hub, tmp_path):
    s, root = _session(tmp_path, hub)
    # Puzzle is not installed and has no update.
    s.key("down")
    s.wait_ms(180)
    before = s.snapshot("before.ppm")
    s.key("delete")
    s.wait_ms(180)
    after = s.snapshot("after.ppm")
    s.quit()
    s.run()
    assert _same(before, after), "Del opened remove for a not-installed app"
    assert not os.path.exists(os.path.join(root, "C", "APPS",
                                           "puzzle.APP"))


def test_offline_error_and_esc_leave(native_mmcore, tmp_path):
    import socket

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    root = os.path.join(str(tmp_path), "root")
    os.makedirs(os.path.join(root, "C", "APPS"), exist_ok=True)
    work = os.path.join(str(tmp_path), "work")
    s = NativeSession(work, env={
        "MMB_DRIVE_ROOT": root,
        "MMCORE_RELEASE_VERSION": "0.312.0",
    })
    s.feed('OPTION HUB URL "http://127.0.0.1:%d/"' % port)
    s.feed("MODE 9,16")
    s.feed("HUB")
    s.wait_ms(1400)
    s.start()
    err = s.snapshot("err.ppm")
    red = 0
    for y in range(0, err.height, 3):
        for x in range(0, err.width, 3):
            r, g, b = err.pixel(x, y)
            if r > 150 and g < 120 and b < 120:
                red += 1
    assert red > 40, "loading screen did not show the red error state"
    s.key("esc")
    s.wait_ms(200)
    left = s.snapshot("left.ppm")
    s.quit()
    out = s.run()
    assert err.data != left.data, "Esc did not leave the HUB screen"
    assert "?HUB" not in out, out


def test_install_then_run(native_mmcore, hub, tmp_path):
    s, root = _session(tmp_path, hub)
    path = os.path.join(root, "C", "APPS", "puzzle.APP")
    assert not os.path.exists(path)
    s.key("down")
    s.wait_ms(150)
    s.key("enter")
    s.wait_ms(250)
    s.key("enter")          # INSTALL on the detail page
    s.wait_ms(600)
    installed = s.mark("installed")
    s.wait_mark(installed)
    # The install input pump may consume harness steps while the transfer is
    # still unwinding, so poll the file instead of assuming the mark implies
    # the rename has completed.
    assert _wait_file(path), "install did not write puzzle.APP"
    s.key("enter")          # the main button is now RUN
    s.wait_ms(400)
    ran = s.mark("ran")
    s.wait_mark(ran)
    s.quit()
    out = s.wait(timeout=30)
    assert "PUZZLE RAN" in out, out


def _bulk_session(tmp_path, hub, delay=0.03):
    """HUB with a single 8 KB app whose body is served in throttled chunks."""
    hub.add_index("/index.json", index_doc(apps=[app_entry("bulk")]))
    hub.throttle("/apps/bulk.app", delay)
    return _session(tmp_path, hub, preinstalled=())


def _tmp_leftovers(apps_dir):
    if not os.path.isdir(apps_dir):
        return []
    return [n for n in os.listdir(apps_dir) if n.endswith(".tmp")]


def test_esc_cancels_inflight_install(native_mmcore, hub, tmp_path):
    s, root = _bulk_session(tmp_path, hub)
    apps_dir = os.path.join(root, "C", "APPS")
    app = os.path.join(apps_dir, "bulk.APP")

    s.key("enter")          # open the detail page
    s.wait_ms(250)
    detail = s.snapshot("detail.ppm")
    s.key("enter")          # start the blocking INSTALL
    s.key("esc")            # cancelled by the install input pump
    # A following text byte makes the front-end screenshot decoder replay the
    # buffered Esc immediately instead of waiting for its idle timeout, so the
    # cancel does not race the transfer. The no-op moves soak up one harness
    # step per progress callback before the scripted marker runs.
    s.text("x")
    s.move(0, 0)
    s.move(0, 0)
    s.move(0, 0)
    done = s.mark("cancelled")
    s.wait_mark(done)
    back = s.snapshot("back.ppm")
    s.quit()
    out = s.wait(timeout=30)

    assert not os.path.exists(app), "cancel still wrote bulk.APP"
    assert _tmp_leftovers(apps_dir) == [], _tmp_leftovers(apps_dir)
    assert _same(detail, back), "cancel did not return to the detail page"
    assert "?HUB" not in out, out


def test_cancel_button_stops_install(native_mmcore, hub, tmp_path):
    s, root = _bulk_session(tmp_path, hub)
    apps_dir = os.path.join(root, "C", "APPS")
    app = os.path.join(apps_dir, "bulk.APP")

    s.key("enter")
    s.wait_ms(250)
    detail = s.snapshot("detail.ppm")
    s.key("enter")          # start the blocking INSTALL
    # MODE 9 is 1024x768. The dialog is 62% by 64% and the CANCEL
    # button sits on its bottom edge; the centre is (512, 598).
    s.move(512, 598)
    s.down("l")
    s.up("l")
    s.move(0, 0)
    s.move(0, 0)
    s.move(0, 0)
    done = s.mark("cancelled")
    s.wait_mark(done)
    back = s.snapshot("back.ppm")
    s.quit()
    out = s.wait(timeout=30)

    assert not os.path.exists(app), "CANCEL still wrote bulk.APP"
    assert _tmp_leftovers(apps_dir) == [], _tmp_leftovers(apps_dir)
    assert _same(detail, back), "CANCEL did not return to the detail page"
    assert "?HUB" not in out, out


def _orange(ppm):
    """Pixels in the size-chip / WHAT'S NEW orange, not the gold frame."""
    d = ppm.data
    n = 0
    for i in range(0, len(d), 3):
        r, g, b = d[i], d[i + 1], d[i + 2]
        if r > 230 and 130 < g < 190 and b < 80:
            n += 1
    return n


def test_sparse_metadata_hides_empty_fields(native_mmcore, hub, tmp_path):
    """Empty version, author, summary and size draw nothing, and WHAT'S NEW
    appears only when the index carries a changes blurb."""
    sparse = app_entry("puzzle")
    sparse.update({
        "id": "sparse",
        "title": "Sparse",
        "author": "",
        "version": "",
        "summary": "",
        "size": 0,
        "category": "GAMES",
        "about": "Just the description.",
        "changes": "",
        "icon": "",
        "thumb": "",
        "shots": [],
    })
    rich = app_entry("puzzle", category="GAMES")
    rich["changes"] = "New rotation. Faster drop."
    hub.add_index("/index.json", index_doc(apps=[sparse, rich]))

    s, _ = _session(tmp_path, hub, preinstalled=())
    s.key("enter")
    s.wait_ms(400)
    bare = s.snapshot("bare.ppm")
    s.key("esc")
    s.wait_ms(200)
    s.key("down")
    s.wait_ms(150)
    s.key("enter")
    s.wait_ms(400)
    full = s.snapshot("full.ppm")
    s.quit()
    out = s.run()

    assert "?HUB" not in out, out
    assert not _same(bare, full)
    assert _orange(bare) < 30, _orange(bare)
    assert _orange(full) > _orange(bare) + 40
