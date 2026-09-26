"""PrtScr/Ctrl-C can stop RUN; REBOOT is documented (not executed)."""

import socket
import threading
import time

import pytest

from harness import MMBasicConsole, qemu_usb_net_args
from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all
from net_util import live_net_enabled, require_host_tcp
from test_qemu_ethernet import _wait_dhcp


def _usb_console(kernel_image) -> MMBasicConsole:
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


def test_help_reboot(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "REBOOT" in listing
    close_ihelp(console)
    out = dump_topic(console, "REBOOT")
    assert out != "?SYNTAX ERROR"
    assert "REBOOT" in out
    assert "reset" in out.lower() or "watchdog" in out.lower()
    assert "Ctrl+Alt+Del" in out or "ctrl+alt+del" in out.lower()
    alias = dump_topic(console, "RESTART")
    assert "REBOOT" in alias
    run = dump_topic(console, "RUN")
    assert "PrtScr" in run or "Print Screen" in run
    assert "BREAK" in run


def test_ctrl_c_breaks_running_program(fresh_console):
    c = fresh_console
    assert c.send_line("10 PAUSE 20") == ""
    assert c.send_line("20 GOTO 10") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT 1+1") == "2"


def test_ctrl_c_stops_play(fresh_console):
    c = fresh_console
    assert c.send_line("PLAY TONE 440, 440") == ""
    assert c.send_line("PRINT PLAYING()") == "1"
    assert c.send_line("10 PAUSE 20") == ""
    assert c.send_line("20 GOTO 10") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT PLAYING()") == "0"


def test_ctrl_c_page_write_loop_restores_console(fresh_console):
    c = fresh_console
    assert c.send_line("NEW") == ""
    assert c.send_line("10 PAGE WRITE 1") == ""
    assert c.send_line("20 PAGE DISPLAY 1") == ""
    assert c.send_line("30 CLS") == ""
    assert c.send_line("40 GOTO 30") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT 9") == "9"
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("CLS RGB(255,0,0)") == ""
    pix = int(c.send_line("PRINT PIXEL(4,4)"))
    assert ((pix >> 16) & 255) > 150


def _ctrl_alt_del(con: MMBasicConsole) -> None:
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(0.15)
    con.key_down("delete")
    time.sleep(0.3)
    con.key_up("delete")
    time.sleep(0.15)
    con.key_up("alt")
    con.key_up("ctrl")


def _wait_for_mmbasic(con: MMBasicConsole, timeout: float = 12.0) -> str:
    seen = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        seen += con.drain(quiet=0.3).decode(errors="ignore")
        if "MMBasic" in seen:
            break
    return seen


def test_ctrl_alt_del_returns_to_prompt(kernel_image):
    """#577: Ctrl+Alt+Del is a warm reset. It must land at a ready prompt with
    the interpreter state cleared, not leave the session without a REPL."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("A = 1234") == ""
        assert con.send_line("PRINT A") == "1234"

        con.drain(quiet=0.2)
        _ctrl_alt_del(con)
        assert "MMBasic" in _wait_for_mmbasic(con)

        # Fresh session: variables and the program are gone.
        assert con.send_line("PRINT A") == "0"
        assert con.send_line("PRINT 6 * 7") == "42"
    finally:
        con.stop()


def _open_app(con: MMBasicConsole, command: str) -> bytes:
    con.drain(quiet=0.2)
    con._ser.sendall(command.encode() + b"\r")
    return con.drain(quiet=0.8)


def test_ctrl_alt_del_closes_fullscreen_app(kernel_image):
    """#606: The warm reset keeps the interpreter object in place, so a
    full-screen app that owns the keyboard must be closed too, or the REPL
    never becomes responsive again."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        _open_app(con, "FILES")

        _ctrl_alt_del(con)
        assert "MMBasic" in _wait_for_mmbasic(con)
        assert con.send_line("PRINT 6 * 7") == "42"
    finally:
        con.stop()


def test_ctrl_alt_del_closes_settings_app(kernel_image):
    """#606: SETTINGS is a modal TUI that blocks the REPL (the same class of
    full-screen app as FILES)."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        _open_app(con, "SETTINGS")

        _ctrl_alt_del(con)
        assert "MMBasic" in _wait_for_mmbasic(con)
        assert con.send_line("PRINT 6 * 7") == "42"
    finally:
        con.stop()


def _switch_console(con: MMBasicConsole, n: int) -> None:
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(0.15)
    con.key_down(str(n))
    time.sleep(0.25)
    con.key_up(str(n))
    time.sleep(0.15)
    con.key_up("alt")
    con.key_up("ctrl")
    time.sleep(0.5)


def test_ctrl_alt_del_closes_app_on_background_console(kernel_image):
    """#763: a full-screen app left on an inactive virtual console must not
    survive the warm reset. Every console comes back at a ready prompt."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        _open_app(con, "FILES")
        _switch_console(con, 2)
        con.drain(quiet=0.3, timeout=3.0)

        con.drain(quiet=0.2)
        _ctrl_alt_del(con)
        assert "MMBasic" in _wait_for_mmbasic(con)

        # Console 1 held the app when the reset happened; it must be gone.
        _switch_console(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT 6 * 7") == "42"

        # And console 2, active at reset time, is still usable.
        _switch_console(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT 7 * 7") == "49"
    finally:
        con.stop()


class _HostTcpProbe:
    """A host listener that records payload and signals peer close."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(30.0)
        self.port = self.sock.getsockname()[1]
        self.closed = threading.Event()
        self.data = bytearray()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        conn.settimeout(0.25)
        try:
            while True:
                try:
                    chunk = conn.recv(256)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                self.data.extend(chunk)
        finally:
            try:
                conn.close()
            except OSError:
                pass
            self.closed.set()

    def wait_data(self, needle: bytes, timeout: float = 8.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in bytes(self.data):
                return True
            time.sleep(0.05)
        return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


@pytest.fixture(scope="module")
def tcp_reset_console(kernel_image):
    """QEMU with USB Ethernet and a USB keyboard (for Ctrl+Alt+Del/2)."""
    if not live_net_enabled():
        pytest.skip("set MMCORE_LIVE_NET=1 to run live TCP reset tests")
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args() + ["-device", "usb-kbd"],
        boot_timeout=40.0,
    )
    con.start()
    yield con
    con.stop()


@pytest.mark.skipif(
    not live_net_enabled(),
    reason="set MMCORE_LIVE_NET=1 to run live guest<->host TCP tests",
)
def test_warm_reset_closes_tcp_owned_by_background_console(tcp_reset_console):
    """#785: a TCP file opened on console 1 must be closed by a warm reset even
    after the user switched to console 2, instead of leaking the machine-wide
    socket with no owning console."""
    con = tcp_reset_console
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = con.send_line("OPTION ETHERNET ON", timeout=25)
    if "10.0.2." not in out:
        out = out + "\n" + _wait_dhcp(con)
    assert "10.0.2." in out or "link is up" in out.lower(), out
    require_host_tcp(con)

    probe = _HostTcpProbe()
    try:
        opened = con.send_line(
            'OPEN "TCP:10.0.2.2:%d" AS #1' % probe.port, timeout=25
        )
        low = opened.lower()
        assert "network not available" not in low, opened
        assert "tcp timeout" not in low and "tcp refused" not in low, opened
        assert con.send_line('PRINT #1, "PING"') == ""
        assert probe.wait_data(b"PING"), "guest never reached the host listener"

        # Hand the socket to console 1, then make console 2 active.
        _switch_console(con, 2)
        con.drain(quiet=0.3, timeout=3.0)
        assert con.send_line("PRINT 2") == "2"

        # Warm reset from console 2: console 1's file table must be closed too,
        # not just the (empty) active one.
        con.drain(quiet=0.2)
        _ctrl_alt_del(con)
        assert "MMBasic" in _wait_for_mmbasic(con)

        assert probe.closed.wait(timeout=10.0), (
            "TCP socket leaked: the host connection stayed open after warm reset"
        )
    finally:
        probe.close()
        con.send_line("CLOSE #1")
