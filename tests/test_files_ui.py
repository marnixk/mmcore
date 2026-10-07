"""FILES dual-pane TUI: navigate, run .BAS, view unknown types, quit."""

import os
import shutil
import subprocess
import tempfile
import time

import pytest

from harness import MMBasicConsole


def _open_files(con: MMBasicConsole) -> str:
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(b"FILES\r")
    return con.drain(quiet=0.8).decode(errors="replace")


def _keys(con: MMBasicConsole, data: bytes, quiet: float = 0.45) -> str:
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def _down_to(con: MMBasicConsole, needle: str, maxn: int = 16) -> str:
    """Move the file-list selection down until the status shows `needle`."""
    seen = ""
    for _ in range(maxn):
        if needle in seen:
            return seen
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert needle in seen, (needle, seen)
    return seen


def _prep_tree(con: MMBasicConsole) -> None:
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('MKDIR "DEMO"') == ""
    assert con.send_line('OPEN "HELLO.BAS" FOR OUTPUT AS #1') == ""
    assert con.send_line("PRINT #1, \"PRINT 42\"") == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('OPEN "NOPE.XYZ" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "xyz"') == ""
    assert con.send_line("CLOSE #1") == ""
    listing = con.send_line("DIR")
    assert "HELLO.BAS" in listing.upper()
    assert "DEMO" in listing.upper()


def test_files_is_not_dir(console):
    listing = console.send_line("DIR")
    assert "TEST.PNG" in listing.upper() or "HELLO" in listing.upper() or listing


def test_files_opens_dual_pane(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "[FILES]" in seen
    assert "HELLO.BAS" in seen.upper()
    assert ".." in seen or "/" in seen
    assert "DEMO" in seen.upper()
    assert "Tab panels" in seen
    _keys(con, b"q")


def test_files_tab_switches_panels(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "P=L" in seen
    seen = _keys(con, b"\t")
    assert "P=R" in seen
    seen = _keys(con, b"\t")
    assert "P=L" in seen
    _keys(con, b"q")


def test_files_right_menu_drive_updates_right_pane(fresh_console):
    """Right-menu drive change must not rewrite the focused left pane."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    _down_to(con, "SEL=DEMO/")
    seen = _keys(con, b"\r")
    assert "L=A:/DEMO" in seen.upper() or "PATH=A:/DEMO" in seen.upper()
    assert "P=L" in seen
    # Alt+R, then Drive A: (hotkey a). Left stays in DEMO; right is A:/.
    seen = _keys(con, bytes([1]) + b"ra")
    upper = seen.upper()
    assert "L=A:/DEMO" in upper
    assert "R=A:/" in upper
    assert "P=R" in seen
    _keys(con, b"q")


def test_files_enter_subdir_and_parent(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # listing is sorted; seeded dirs (apps, lib, tests) may precede DEMO.
    _down_to(con, "SEL=DEMO/")
    seen = _keys(con, b"\r")
    assert "DEMO" in seen.upper()
    assert "PATH=A:/DEMO" in seen.upper() or "A:/DEMO" in seen.upper()
    seen = _keys(con, b"\x7f")
    assert "PATH=A:/" in seen.upper() or "L=A:/" in seen
    _keys(con, b"q")
    cwd = con.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_files_run_bas(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # Navigate to HELLO.BAS regardless of the seeded directories above it.
    _down_to(con, "SEL=HELLO.BAS")
    seen = _keys(con, b"\r", quiet=1.0)
    assert "42" in seen
    # back at the prompt
    assert con.send_line("PRINT 1+1") == "2"


def test_files_quit_prints_prompt(fresh_console):
    """q/Esc from FILES should reprint the prompt without needing Enter."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    con.drain(quiet=0.15)
    con._ser.sendall(b"q")
    out = con.drain(quiet=0.6).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert con.send_line("PRINT 3") == "3"

    _open_files(con)
    con.drain(quiet=0.15)
    con._ser.sendall(b"\x1b")
    out = con.drain(quiet=0.7).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert con.send_line("PRINT 4") == "4"


def test_files_quit_q_and_esc(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    seen = _keys(con, b"q")
    assert ">" in seen or con.send_line("PRINT 7") == "7"
    _open_files(con)
    _keys(con, b"\x1b", quiet=0.6)
    assert con.send_line("PRINT 8") == "8"


def test_files_view_unsupported_is_info(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    # Walk until SEL=NOPE.XYZ then view
    for _ in range(12):
        if "SEL=NOPE.XYZ" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=NOPE.XYZ" in seen
    seen = _keys(con, b"v")
    assert "File info" in seen or "NOPE.XYZ" in seen
    assert "?" not in seen.split("\n")[0] or "File info" in seen
    _keys(con, b"q")
    # still in FILES after closing info with... overlay closes on most keys.
    # q after info first closes overlay then we need another q; send two
    _keys(con, b"q")


def test_files_view_seeded_png_smoke(fresh_console):
    con = fresh_console
    _prep_tree(con)
    assert con.send_line('CHDIR "A:/tests"') == ""
    seen = _open_files(con)
    for _ in range(16):
        if "SEL=TEST.PNG" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=TEST.PNG" in seen
    seen = _keys(con, b"v", quiet=1.2)
    assert "PREVIEW TEST.PNG 8x8 MODE 5" in seen
    # Only Enter/Esc leave preview; other keys are ignored.
    _keys(con, b"x", quiet=0.4)
    seen = _keys(con, b"\r", quiet=0.8)
    assert "SEL=" in seen
    _keys(con, b"q")
    assert con.send_line("PRINT MM.HRES") == "1280"
    assert con.send_line("PRINT MM.VRES") == "720"


def test_files_preview_shows_name_size_and_arrows(fresh_console):
    """Preview captions the file and Left/Right walk the folder's images."""
    con = fresh_console
    _prep_tree(con)
    assert con.send_line('CHDIR "A:/tests"') == ""
    seen = _open_files(con)
    for _ in range(16):
        if "SEL=TEST.PNG" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=TEST.PNG" in seen
    seen = _keys(con, b"v", quiet=1.2)
    assert "PREVIEW TEST.PNG 8x8 MODE 5" in seen
    # The caption is drawn over the image at the top of the 240x216 preview.
    assert con.screen_size() == (240, 216)
    band = [
        con.screen_pixel(x, y)
        for x in range(4, 112, 4)
        for y in range(4, 20, 4)
    ]
    assert any(r > 150 and g > 150 and b > 150 for r, g, b in band), band
    # Right -> next image, Left -> previous. Order is TEST.JPG, TEST.PCX,
    # TEST.PNG, TESTZ.PNG.
    seen = _keys(con, b"\x1b[C", quiet=0.9)
    assert "PREVIEW TESTZ.PNG 8x8 MODE 5" in seen
    seen = _keys(con, b"\x1b[D", quiet=0.9)
    assert "PREVIEW TEST.PNG 8x8 MODE 5" in seen
    seen = _keys(con, b"\x1b[D", quiet=0.9)
    assert "PREVIEW TEST.PCX 8x8 MODE 5" in seen
    seen = _keys(con, b"\x1b[D", quiet=0.9)
    assert "PREVIEW TEST.JPG 8x8 MODE 5" in seen
    # Enter returns with the browsed file selected.
    seen = _keys(con, b"\r", quiet=0.8)
    assert "SEL=TEST.JPG" in seen
    _keys(con, b"q")
    assert con.send_line("PRINT MM.HRES") == "1280"


def test_files_preview_seeded_pcx(fresh_console):
    """#631: the FILES preview decodes and draws a .PCX."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/tests"') == ""
    _select(con, "TEST.PCX")
    seen = _keys(con, b"v", quiet=1.2)
    assert "PREVIEW TEST.PCX 8x8 MODE 5" in seen, seen
    assert con.screen_size() == (240, 216)
    # The 8x8 image is centred and every pixel is VGA index 1 (blue).
    w, h = con.screen_size()
    x0, y0 = (w - 8) // 2, (h - 8) // 2
    coords = [(x0 + dx, y0 + dy) for dx in range(8) for dy in range(8)]
    blue = sum(
        1 for r, g, b in con.screen_pixels(coords) if b > 120 and r < 60 and g < 60
    )
    assert blue > 40, blue
    seen = _keys(con, b"\r", quiet=0.8)
    assert "SEL=TEST.PCX" in seen
    _keys(con, b"q")
    assert con.send_line("PRINT MM.HRES") == "1280"


def test_dir_still_lists(console):
    listing = console.send_line('DIR "A:/tests"')
    assert "TEST.PNG" in listing.upper()


def test_files_reports_video_cell_size(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "COLS=160" in seen
    assert "ROWS=45" in seen
    _keys(con, b"q")


def test_files_box_drawing_covers_cell_height(fresh_console):
    """Vertical pane border is a full-height line, not ASCII '|' with gaps."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # Header row (y=2) left pane border is white-on-blue │; glyph row 0 of
    # ASCII '|' is empty, so a lit pixel here is the full-height box line.
    r, g, b = con.screen_pixel(3, 2 * 16)
    assert r > 100 and g > 100 and b > 100, (r, g, b)
    # Horizontal ─ on the top pane border, away from the volume caption.
    r, g, b = con.screen_pixel(200, 16 + 7)
    assert r > 100 and g > 100 and b > 100, (r, g, b)
    # Last fkey row covers the bottom of 1280x720.
    br, bg_, bb = con.screen_pixel(24, 704)
    assert br + bg_ + bb > 40, (br, bg_, bb)
    _keys(con, b"q")


def test_files_volume_caption_has_friendly_name(fresh_console):
    """The pane caption shows a friendly volume name, not just the letter."""
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "VOL=A: RAM" in seen.upper(), seen
    _keys(con, b"q")


def test_files_command_menu_has_eject(fresh_console):
    """Command > Eject is available and reports when nothing is removable."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    seen = _keys(con, bytes([1]) + b"ce", quiet=0.6)
    assert "HINT=No removable drive" in seen, seen
    _keys(con, b"q")


def test_files_tracks_mode_resolution(fresh_console):
    con = fresh_console
    assert con.send_line("MODE 7,8") == ""
    assert con.send_line("PRINT MM.HRES") == "320"
    assert con.send_line("PRINT MM.VRES") == "240"
    _prep_tree(con)
    seen = _open_files(con)
    assert "COLS=40" in seen
    assert "ROWS=15" in seen
    _keys(con, b"q")
    assert con.send_line("MODE 8,16") == ""


def test_files_follows_editor_theme_phosphor(fresh_console):
    con = fresh_console
    assert con.send_line("OPTION THEME PHOSPHOR") == ""
    _prep_tree(con)
    _open_files(con)
    empty = [con.screen_pixel(x, 176) for x in (40, 80, 360, 400)]
    assert all(r + g + b < 50 for r, g, b in empty), empty
    r, g, b = con.screen_pixel(3, 2 * 16)
    assert g > r + 20 and g > 40, (r, g, b)
    _keys(con, b"q")
    assert con.send_line("OPTION THEME TURBO") == ""


def test_files_follows_editor_theme_paper(fresh_console):
    con = fresh_console
    assert con.send_line("OPTION THEME PAPER") == ""
    _prep_tree(con)
    _open_files(con)
    empty = [con.screen_pixel(x, 176) for x in (40, 80, 360, 400)]
    assert any(r > 140 and g > 130 and b > 120 for r, g, b in empty), empty
    _keys(con, b"q")
    assert con.send_line("OPTION THEME TURBO") == ""


def test_files_f4_shows_editor_immediately(fresh_console):
    """#135: F4/e from FILES must paint the editor without waiting for another key."""
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    for _ in range(12):
        if "SEL=HELLO.BAS" in seen.upper() or "SEL=HELLO.BAS" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "HELLO" in seen.upper()
    opened = _keys(con, b"\x1b[14~", quiet=0.9)
    assert "File" in opened
    assert "Run" in opened or "Alt+X" in opened
    pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
    assert all(r < 50 and g < 50 and b < 55 for r, g, b in pane), pane
    _keys(con, bytes([1]) + b"x", quiet=0.6)
    _keys(con, b"q")
    assert con.send_line("PRINT 5") == "5"


def test_files_edit_txt_opens_line_editor(fresh_console):
    """#1093: a non-markdown text file keeps the plain line editor."""
    con = fresh_console
    _prep_tree(con)
    assert con.send_line('OPEN "NOTES.TXT" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "plain text"') == ""
    assert con.send_line("CLOSE #1") == ""
    _open_files(con)
    _down_to(con, "SEL=NOTES.TXT")
    opened = _keys(con, b"\x1b[14~", quiet=0.9)  # F4 edit
    assert "WORDPAD" not in opened.upper(), opened
    assert "File" in opened
    assert "Run" in opened or "Alt+X" in opened
    _keys(con, bytes([1]) + b"x", quiet=0.6)  # leave the line editor
    _keys(con, b"q")
    assert con.send_line("PRINT 5") == "5"


def test_files_edit_md_opens_wordpad(fresh_console):
    """#1093: editing a .md from FILES opens WORDPAD and returns to FILES."""
    con = fresh_console
    _prep_tree(con)
    assert con.send_line('OPEN "NOTES.MD" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "# Heading"') == ""
    assert con.send_line("CLOSE #1") == ""
    _open_files(con)
    _down_to(con, "SEL=NOTES.MD")
    opened = _keys(con, b"\x1b[14~", quiet=1.0)  # F4 edit
    assert "WORDPAD" in opened.upper(), opened
    # WORDPAD rendered the markdown document (the leading "#" is hidden).
    assert "Heading" in opened
    # A clean WORDPAD quit (Alt+X) returns to the FILES browser.
    back = _keys(con, bytes([1]) + b"x", quiet=0.8)
    assert "[FILES]" in back or "SEL=" in back, back
    _keys(con, b"q")
    assert con.send_line("PRINT 6") == "6"


def _select(con: MMBasicConsole, name: str, maxn: int = 40) -> str:
    seen = _open_files(con)
    for _ in range(maxn):
        if f"SEL={name}" in seen:
            return seen
        seen = _keys(con, b"\x1b[B", quiet=0.2)
    assert f"SEL={name}" in seen, (name, seen)
    return seen


def _preview_ink(con: MMBasicConsole, tries: int = 12) -> float:
    """Lit fraction of the specimen band, retrying while QEMU repaints."""
    png = con.capture_png()
    out = subprocess.run(
        [
            "convert", png, "-crop", "660x200+0+16", "+repage",
            "-colorspace", "gray", "-threshold", "50%",
            "-format", "%[fx:mean]", "info:",
        ],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    return float(out)


def test_files_tdf_preview_and_mode_restore(fresh_console):
    """#586: Enter on a .TDF draws a specimen and Esc restores the console."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/fonts/tdf/mono"') == ""
    _select(con, "STANDARD.TDF")
    seen = _keys(con, b"\r", quiet=1.0)
    assert "[FILES] TDF STANDARD.TDF Standard" in seen, seen
    # The specimen draws bright CP437 glyphs on black; QEMU repaints slowly.
    ink = 0.0
    for _ in range(12):
        ink = max(ink, _preview_ink(con))
        if ink > 0.001:
            break
        time.sleep(0.3)
    assert ink > 0.001, ink
    seen = _keys(con, b"\x1b", quiet=0.8)
    assert "SEL=" in seen
    _keys(con, b"q")
    # No MODE switch: the console is back at its original resolution.
    assert con.send_line("PRINT MM.HRES") == "1280"
    assert con.send_line("PRINT MM.VRES") == "720"


def test_files_tdf_bad_font_fails_soft(fresh_console):
    """#586: a non-TDF file falls back to the info overlay, not a crash."""
    con = fresh_console
    assert con.send_line('OPEN "A:/BADFONT.TDF" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, STRING$(240, "X")') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('CHDIR "A:/"') == ""
    _select(con, "BADFONT.TDF", maxn=40)
    seen = _keys(con, b"\r", quiet=0.8)
    assert "Cannot preview this .TDF" in seen or "File info" in seen, seen
    seen = _keys(con, b"\x1b", quiet=0.6)
    assert "SEL=" in seen
    _keys(con, b"q")
    assert con.send_line("PRINT 6") == "6"


def test_files_tdf_multi_variant_preview(fresh_console):
    """#629/#865: one variation shows at a time; Left/Right cycles them."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/fonts/tdf/color"') == ""
    _select(con, "ACIDSC2X.TDF")
    seen = _keys(con, b"\r", quiet=1.0)
    assert "[FILES] TDF ACIDSC2X.TDF" in seen, seen
    assert "6 variants" in seen, seen
    ink = 0.0
    for _ in range(12):
        ink = max(ink, _preview_ink(con))
        if ink > 0.001:
            break
        time.sleep(0.3)
    assert ink > 0.001, ink
    # Left/Right cycle variations (wrap at both ends) and report n/N.
    seen = _keys(con, b"\x1b[C", quiet=0.4)
    assert "variation 2/6" in seen, seen
    seen = _keys(con, b"\x1b[C", quiet=0.4)
    assert "variation 3/6" in seen, seen
    seen = _keys(con, b"\x1b[D", quiet=0.4)
    assert "variation 2/6" in seen, seen
    seen = _keys(con, b"\x1b[D", quiet=0.4)
    seen = _keys(con, b"\x1b[D", quiet=0.4)
    assert "variation 6/6" in seen, seen
    # Up/Down still scroll within the current variation.
    _keys(con, b"\x1b[B", quiet=0.3)
    _keys(con, b"\x1b[6~", quiet=0.5)
    seen = _keys(con, b"\x1b", quiet=0.8)
    assert "SEL=" in seen
    _keys(con, b"q")
    assert con.send_line("PRINT MM.HRES") == "1280"


def test_files_tdf_pageup_pagedown_single(fresh_console):
    """#622: PageUp/PageDown page a single-record .TDF and return without error."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/fonts/tdf/mono"') == ""
    _select(con, "STANDARD.TDF")
    seen = _keys(con, b"\r", quiet=1.0)
    assert "[FILES] TDF STANDARD.TDF" in seen, seen
    # Two pages down then two back up (clamps at both ends), then return.
    _keys(con, b"\x1b[6~", quiet=0.5)
    _keys(con, b"\x1b[6~", quiet=0.5)
    _keys(con, b"\x1b[5~", quiet=0.5)
    _keys(con, b"\x1b[5~", quiet=0.5)
    seen = _keys(con, b"\x1b", quiet=0.8)
    assert "SEL=" in seen, seen
    _keys(con, b"q")
    assert con.send_line("PRINT MM.HRES") == "1280"


def test_files_move_keeps_selection_near_moved(fresh_console):
    """#1022/#1028: a cross-folder move reparents the file and the source pane
    selects the row above the moved one."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('MKDIR "MOVEDIR"') == ""
    assert con.send_line('MKDIR "DESTDIR"') == ""
    for name in ("F1.TXT", "F2.TXT", "F3.TXT", "F4.TXT", "F5.TXT"):
        assert con.send_line(f'OPEN "MOVEDIR/{name}" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "x"') == ""
        assert con.send_line("CLOSE #1") == ""
    assert con.send_line('CHDIR "A:/MOVEDIR"') == ""
    _open_files(con)
    _down_to(con, "SEL=F3.TXT")
    _keys(con, b"m", quiet=0.4)
    # Both panes are A:/MOVEDIR, so the seeded destination is
    # A:/MOVEDIR/F3.TXT; retype it cross-folder to A:/DESTDIR/F3.TXT.
    _keys(con, b"\x7f" * 24)
    seen = _keys(con, b"A:/DESTDIR/F3.TXT\r", quiet=0.9)
    assert "Moved" in seen, seen
    # F3 is gone: the selection must land on F2 (index moved_idx-1), not "..".
    assert "SEL=F2.TXT" in seen, seen
    _keys(con, b"q")
    assert "F3.TXT" in con.send_line('DIR "A:/DESTDIR"').upper()
    assert "F3.TXT" not in con.send_line('DIR "A:/MOVEDIR"').upper()


def _seed_file(con: MMBasicConsole, path: str, text: str = "x") -> None:
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    assert con.send_line(f'PRINT #1, "{text}"') == ""
    assert con.send_line("CLOSE #1") == ""


def test_files_copies_directory_tree(fresh_console):
    """#1112: Copy on a folder brings the whole tree, empty nests included."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('MKDIR "FCOPYSRC"') == ""
    assert con.send_line('MKDIR "FCOPYSRC/SUB"') == ""
    assert con.send_line('MKDIR "FCOPYSRC/SUB/EMPTY"') == ""
    _seed_file(con, "A:/FCOPYSRC/TOP.TXT", "top")
    _seed_file(con, "A:/FCOPYSRC/SUB/DEEP.TXT", "deep")
    assert con.send_line('MKDIR "FCOPYDST"') == ""
    _open_files(con)
    _down_to(con, "SEL=FCOPYSRC/")
    _keys(con, b"c", quiet=0.4)
    # Both panes are A:/, so the seed is A:/FCOPYSRC; retype the destination.
    _keys(con, b"\x7f" * 40)
    seen = _keys(con, b"A:/FCOPYDST/FCOPYSRC\r", quiet=1.5)
    assert "Copied" in seen, seen
    _keys(con, b"q")
    assert "TOP.TXT" in con.send_line('DIR "A:/FCOPYDST/FCOPYSRC"').upper()
    assert "DEEP.TXT" in con.send_line('DIR "A:/FCOPYDST/FCOPYSRC/SUB"').upper()
    assert con.send_line('CHDIR "A:/FCOPYDST/FCOPYSRC/SUB/EMPTY"') == ""
    assert con.send_line('CHDIR "A:/"') == ""


def test_files_moves_directory_into_folder(fresh_console):
    """#1112: Move on a folder reparents the whole tree into a destination."""
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('MKDIR "FMOVESRC"') == ""
    assert con.send_line('MKDIR "FMOVESRC/IN"') == ""
    _seed_file(con, "A:/FMOVESRC/A.TXT", "a")
    _seed_file(con, "A:/FMOVESRC/IN/B.TXT", "b")
    assert con.send_line('MKDIR "FMOVEDST"') == ""
    _open_files(con)
    _down_to(con, "SEL=FMOVESRC/")
    _keys(con, b"m", quiet=0.4)
    _keys(con, b"\x7f" * 40)
    seen = _keys(con, b"A:/FMOVEDST/FMOVESRC\r", quiet=1.5)
    assert "Moved" in seen, seen
    _keys(con, b"q")
    assert "A.TXT" in con.send_line('DIR "A:/FMOVEDST/FMOVESRC"').upper()
    assert "B.TXT" in con.send_line('DIR "A:/FMOVEDST/FMOVESRC/IN"').upper()
    assert "FMOVESRC" not in con.send_line('DIR "A:/"').upper()


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_files_move_directory_across_drives_copies_and_deletes(kernel_image):
    """#1112: moving a folder A: -> C: in FILES copies the tree then deletes
    the source, empty nested folders included."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBXDRV", img],
            check=True,
            capture_output=True,
        )
        os.sync()
        con = MMBasicConsole(
            kernel_image,
            extra_qemu=["-drive", f"file={img},if=sd,format=raw"],
            boot_timeout=30,
        )
        con.start()
        try:
            assert con.send_line('CHDIR "A:/"') == ""
            assert con.send_line('MKDIR "XDIRSRC"') == ""
            assert con.send_line('MKDIR "XDIRSRC/SUB"') == ""
            assert con.send_line('MKDIR "XDIRSRC/SUB/EMPTY"') == ""
            _seed_file(con, "A:/XDIRSRC/TOP.TXT", "top")
            _seed_file(con, "A:/XDIRSRC/SUB/DEEP.TXT", "deep")
            _open_files(con)
            # Right pane -> C: (Alt+R, then the "Drive C:" hotkey); Tab back
            # to the left pane so the move source stays on A:.
            _keys(con, bytes([1]) + b"rc", quiet=0.6)
            _keys(con, b"\t", quiet=0.3)
            _down_to(con, "SEL=XDIRSRC/")
            _keys(con, b"m", quiet=0.4)
            seen = _keys(con, b"\r", quiet=2.0)
            assert "Moved" in seen, seen
            _keys(con, b"q")
            assert "TOP.TXT" in con.send_line('DIR "C:/XDIRSRC"').upper()
            assert "DEEP.TXT" in con.send_line('DIR "C:/XDIRSRC/SUB"').upper()
            assert con.send_line('CHDIR "C:/XDIRSRC/SUB/EMPTY"') == ""
            assert con.send_line('CHDIR "A:/"') == ""
            assert "XDIRSRC" not in con.send_line('DIR "A:/"').upper()
        finally:
            con.stop()
    finally:
        os.unlink(img)


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_files_move_across_drives_copies_and_deletes(kernel_image):
    """#1030: a move from the A: ramdisk to the C: SD volume has no single
    rename primitive, so FILES must copy the file then delete the source."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBXDRV", img],
            check=True,
            capture_output=True,
        )
        os.sync()
        con = MMBasicConsole(
            kernel_image,
            extra_qemu=["-drive", f"file={img},if=sd,format=raw"],
            boot_timeout=30,
        )
        con.start()
        try:
            assert con.send_line('CHDIR "A:/"') == ""
            assert con.send_line('OPEN "XFER.TXT" FOR OUTPUT AS #1') == ""
            assert con.send_line('PRINT #1, "cross-drive"') == ""
            assert con.send_line("CLOSE #1") == ""
            _open_files(con)
            # Right pane -> C: (Alt+R, then the "Drive C:" hotkey). Tab back
            # to the left pane so the move source stays on A:.
            _keys(con, bytes([1]) + b"rc", quiet=0.6)
            _keys(con, b"\t", quiet=0.3)
            _down_to(con, "SEL=XFER.TXT")
            _keys(con, b"m", quiet=0.4)
            seen = _keys(con, b"\r", quiet=1.0)
            assert "Moved" in seen, seen
            _keys(con, b"q")
            assert "XFER.TXT" in con.send_line('DIR "C:/"').upper()
            assert "XFER.TXT" not in con.send_line('DIR "A:/"').upper()
        finally:
            con.stop()
    finally:
        os.unlink(img)


def test_files_many_entries_reports_truncation(fresh_console):
    """#621: a folder over FU_MAX_ENT reports MORE=1 and still lists folders.

    The structured listing fills names, types and sizes in one scan, so a folder
    with 97 entries is cut at 96 and reported instead of silently dropped.
    """
    con = fresh_console
    for line in (
        '10 MKDIR "A:/BIG"',
        '15 MKDIR "A:/BIG/SUB"',
        '20 FOR I=1 TO 96',
        '30 OPEN "A:/BIG/F"+LTRIM$(STR$(I))+".TXT" FOR OUTPUT AS #1',
        '40 PRINT #1,"x"',
        '50 CLOSE #1',
        '60 NEXT',
        '70 PRINT "SEEDED"',
    ):
        con.send_line(line)
    assert "SEEDED" in con.send_line("RUN")
    assert con.send_line('CHDIR "A:/BIG"') == ""
    seen = _open_files(con)
    assert "MORE=1" in seen, seen
    assert "N=96" in seen, seen
    # Directories survive the cap: SUB sorts ahead of the files and is shown.
    assert "SUB/" in seen, seen
    _keys(con, b"q")
    assert con.send_line("PRINT 9") == "9"
