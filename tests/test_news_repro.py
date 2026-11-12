"""Repro for the newsfeeds WEB crash (test/news/main.bas).

``WEB(HTTP GET ... AS Type)`` stores the response in a string-pool block.
A body past 16 KB rounds up to the 64 KB size class, which does not fit in a
64 KB slab, and the next pool allocation writes off the end of that slab.
The process dies (SIGSEGV, or abort from a corrupted heap) instead of
returning the TYPE or raising a BASIC error.

The programs under test/news/ hit this on the live articles payload
(~18 KB, chunked). This test uses a local Content-Length body of the same
size so it does not need that host.
"""
import http.server
import os
import shutil
import subprocess
import threading

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def mmb():
    build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = getattr(self.server, "body", b"{}")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass

    def log_message(self, *args):
        pass


@pytest.fixture()
def http_srv():
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    srv.daemon_threads = True
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=5)


def _run(mmb_bin, program, root):
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    return subprocess.run(
        [mmb_bin],
        input=program if program.endswith("\n") else program + "\n",
        text=True,
        capture_output=True,
        timeout=60,
        env=env,
    )


@pytest.mark.xfail(
    reason="WEB body over 16KB overflows the 64KB string slab "
           "(mmbasic/src/util.c pool_alloc_raw); process aborts",
    strict=False,
)
def test_web_typed_get_over_16k_stays_alive(mmb, http_srv, tmp_path):
    """A typed WEB GET of a >16KB JSON object must print OK, not abort."""
    http_srv.body = (
        b'{"n":7,"pad":"' + (b"P" * 20000) + b'"}'
    )
    port = http_srv.server_address[1]
    lines = [
        "type T",
        "  n as integer",
        "end type",
        "function G() as T",
        "  local u as string",
        '  u = "http://127.0.0.1:%d/big"' % port,
        "  G = web(http get u as T)",
        "end function",
        "dim a as T",
        "a = G()",
        "print a.n",
        'print "OK"',
    ]
    program = "\n".join(
        "%d %s" % (10 * (i + 1), line) for i, line in enumerate(lines)
    )
    program += "\nRUN\n"
    proc = _run(mmb, program, tmp_path)
    out = proc.stdout + proc.stderr
    if "network not available" in out:
        pytest.skip("host reports no network lease")
    assert proc.returncode == 0, out[-800:]
    tail = out.split("\x1b")[-1]
    assert "7" in tail and "OK" in tail
