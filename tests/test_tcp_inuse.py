"""#778: the TCP client is one machine-wide socket, so a second console must be
told it is in use instead of silently resetting the first console's connection.

Native regression: the SDL build is scriptable (``MMB_SDL_HARNESS``) and its
POSIX net backend reaches a local 127.0.0.1 listener, so the whole cross-console
path runs without QEMU. Guarded by the native-harness skip rules.
"""
import shutil
import socket
import threading
import time

import pytest

from native_harness import NativeSession, build_native, sdl_backend_available

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def native_mmcore():
    build_native()
    if not sdl_backend_available():
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    return True


class _Listener:
    """A local TCP listener that records accepted connections and their data."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.rx = []  # bytes received per accepted connection
        self._stop = False
        self._thread = threading.Thread(target=self._accept, daemon=True)
        self._thread.start()

    def _accept(self):
        self.sock.settimeout(0.2)
        while not self._stop:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            buf = bytearray()
            self.rx.append(buf)
            threading.Thread(
                target=self._read, args=(c, buf), daemon=True
            ).start()

    @staticmethod
    def _read(conn, buf):
        try:
            while True:
                data = conn.recv(256)
                if not data:
                    break
                buf.extend(data)
        except OSError:
            pass
        finally:
            conn.close()

    def wait_rx(self, idx, expected, timeout=3.0):
        """Return the bytes received on connection ``idx`` once they match."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if bytes(self.rx[idx]) == expected:
                return bytes(self.rx[idx])
            time.sleep(0.02)
        return bytes(self.rx[idx])

    def close(self):
        self._stop = True
        self._thread.join(timeout=2.0)
        self.sock.close()


def _tcp(port):
    return 'TCP:127.0.0.1:%d' % port


def test_second_console_open_reports_in_use(native_mmcore, tmp_path):
    """Console 1 holds the socket; console 2 is refused by name, console 1's
    connection still works, and the same-console file conflict is unchanged."""
    srv = _Listener()
    try:
        s = NativeSession(tmp_path)
        s.feed('OPEN "%s" AS #1' % _tcp(srv.port))
        s.wait_ms(600)
        s.key("ctrl+alt+2")
        s.wait_ms(500)
        s.feed("PRINT 111")
        s.wait_ms(200)
        s.feed('OPEN "%s" AS #2' % _tcp(srv.port))
        s.wait_ms(600)
        s.key("ctrl+alt+1")
        s.wait_ms(500)
        # A second TCP file on the owning console keeps its old ?FILE error.
        s.feed('OPEN "%s" AS #2' % _tcp(srv.port))
        s.wait_ms(400)
        s.feed('PRINT #1, "PING"')
        s.wait_ms(600)
        s.quit()
        out = s.run()
    finally:
        srv.close()

    assert "?IN USE: TCP connection open on console 1" in out, out
    assert "?FILE" in out, out
    assert len(srv.rx) == 1, "console 2 must not reach the net layer"
    assert srv.wait_rx(0, b"PING\n") == b"PING\n"


def test_second_console_connect_reports_in_use(native_mmcore, tmp_path):
    """CONNECT on another console reports the owner instead of resetting it."""
    srv = _Listener()
    try:
        s = NativeSession(tmp_path)
        s.feed('CONNECT "127.0.0.1", %d' % srv.port)
        s.wait_ms(800)
        s.key("ctrl+alt+2")
        s.wait_ms(500)
        s.feed("PRINT 111")
        s.wait_ms(200)
        s.feed('CONNECT "127.0.0.1", %d' % srv.port)
        s.wait_ms(800)
        s.quit()
        out = s.run()
    finally:
        srv.close()

    assert "?IN USE: TCP connection open on console 1" in out, out
    assert len(srv.rx) == 1, "console 2 must not reach the net layer"


def test_second_console_term_reports_in_use(native_mmcore, tmp_path):
    """A TERM-held socket is likewise named when another console tries it."""
    srv = _Listener()
    try:
        s = NativeSession(tmp_path)
        s.feed('TERM "127.0.0.1", %d' % srv.port)
        s.wait_ms(1200)
        s.key("ctrl+alt+2")
        s.wait_ms(600)
        s.feed('OPEN "%s" AS #1' % _tcp(srv.port))
        s.wait_ms(600)
        s.quit()
        out = s.run()
    finally:
        srv.close()

    assert "?IN USE: TCP connection open on console 1" in out, out
    assert len(srv.rx) == 1, "console 2 must not reach the net layer"


def test_new_on_other_console_keeps_socket_owned(native_mmcore, tmp_path):
    """#801: NEW on a console that does not own the shared socket must not
    release it, or the next OPEN on that console resets the owner's live
    connection out from under it."""
    srv = _Listener()
    try:
        s = NativeSession(tmp_path)
        s.feed('OPEN "%s" AS #1' % _tcp(srv.port))
        s.wait_ms(600)
        s.key("ctrl+alt+2")
        s.wait_ms(500)
        s.feed("NEW")
        s.wait_ms(400)
        # Console 2 still must not be able to claim the socket.
        s.feed('OPEN "%s" AS #2' % _tcp(srv.port))
        s.wait_ms(600)
        s.key("ctrl+alt+1")
        s.wait_ms(500)
        # Console 1 still owns it and its connection is intact.
        s.feed('PRINT #1, "PING"')
        s.wait_ms(600)
        s.quit()
        out = s.run()
    finally:
        srv.close()

    assert "?IN USE: TCP connection open on console 1" in out, out
    assert len(srv.rx) == 1, "console 2 must not reach the net layer"
    assert srv.wait_rx(0, b"PING\n") == b"PING\n"


def test_closing_tcp_file_releases_ownership(native_mmcore, tmp_path):
    """CLOSE #1 releases the machine-wide socket for the next console."""
    srv = _Listener()
    try:
        s = NativeSession(tmp_path)
        s.feed('OPEN "%s" AS #1' % _tcp(srv.port))
        s.wait_ms(600)
        s.feed("CLOSE #1")
        s.wait_ms(400)
        s.key("ctrl+alt+2")
        s.wait_ms(500)
        s.feed('OPEN "%s" AS #2' % _tcp(srv.port))
        s.wait_ms(600)
        s.feed('PRINT #2, "PONG"')
        s.wait_ms(600)
        s.quit()
        out = s.run()
    finally:
        srv.close()

    assert "?IN USE" not in out, out
    assert len(srv.rx) == 2, out
    assert srv.wait_rx(1, b"PONG\n") == b"PONG\n"
