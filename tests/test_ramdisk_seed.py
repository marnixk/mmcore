"""A: ramdisk seeding from the embedded ramdisk/ tree."""

from harness import MMBasicConsole


def _read_line(con, path):
    assert con.send_line(f'OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("LINE INPUT #1, A$") == ""
    out = con.send_line("PRINT A$")
    con.send_line("CLOSE #1")
    return out


def _read_head(con, path, n=4):
    assert con.send_line(f'OPEN "{path}" FOR INPUT AS #1') == ""
    lines = []
    for _ in range(n):
        assert con.send_line("LINE INPUT #1, A$") == ""
        lines.append(con.send_line("PRINT A$"))
    con.send_line("CLOSE #1")
    return "\n".join(lines)


def test_ramdisk_seeded_files_present_and_runnable(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        lib = con.send_line('DIR "A:/lib"')
        assert "EXAMPLE.INC" in lib.upper(), lib
        apps = con.send_line('DIR "A:/apps"')
        assert "HELLO.BAS" in apps.upper(), apps
        head = _read_head(con, "A:/lib/EXAMPLE.INC")
        assert "RAMDISK_LIB" in head.upper(), head
        assert con.send_line('RUN "A:/apps/HELLO.BAS"') == "Hello from A:/apps/HELLO.BAS"
    finally:
        con.stop()


def test_ramdisk_seed_is_writable_and_reseed_on_boot(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        original = _read_head(con, "A:/lib/EXAMPLE.INC")
        assert "RAMDISK_LIB" in original.upper(), original
        assert con.send_line('OPEN "A:/lib/EXAMPLE.INC" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "CHANGED"') == ""
        assert con.send_line("CLOSE #1") == ""
        changed = _read_line(con, "A:/lib/EXAMPLE.INC")
        assert changed == "CHANGED", changed
        assert con.send_line('KILL "A:/apps/HELLO.BAS"') == ""
        assert "HELLO.BAS" not in con.send_line('DIR "A:/apps"').upper()
    finally:
        con.stop()

    # A fresh boot re-seeds from the kernel image, restoring the originals.
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        restored = _read_head(con, "A:/lib/EXAMPLE.INC")
        assert "RAMDISK_LIB" in restored.upper(), restored
        assert "HELLO.BAS" in con.send_line('DIR "A:/apps"').upper()
    finally:
        con.stop()
