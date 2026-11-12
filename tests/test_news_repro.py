"""Repro for the newsfeeds WEB crash (test/news/main.bas).

``WEB(HTTP GET ... AS Type)`` stores the response in a string-pool block.
A body past 16 KB rounds up to the 64 KB size class. Before #1501 a 64 KB
slab could not hold that block plus its header, so the next pool allocation
wrote off the end of the slab and the process died (SIGSEGV, or abort from a
corrupted heap) instead of returning the TYPE or raising a BASIC error.

The programs under test/news/ hit this on the live articles payload
(~18 KB, chunked). These tests use local servers so they need no external
network, and exercise both Content-Length and chunked bodies.
"""
import http.server
import json
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
        if self.path.startswith("/api/collections/domains/"):
            body = getattr(self.server, "domains", b"{}")
        elif self.path.startswith("/api/collections/crawl_pages/"):
            body = getattr(self.server, "articles", b"{}")
        else:
            body = getattr(self.server, "body", b"{}")
        chunked = getattr(self.server, "chunked", False)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            if chunked:
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for off in range(0, len(body), 4000):
                    part = body[off:off + 4000]
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.write(b"0\r\n\r\n")
            else:
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


@pytest.mark.parametrize("chunked", [False, True], ids=["content-length", "chunked"])
def test_web_typed_get_over_16k_stays_alive(mmb, http_srv, tmp_path, chunked):
    """A typed WEB GET of a >16KB JSON object must print OK, not abort."""
    http_srv.chunked = chunked
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


def test_news_program_runs_against_mock(mmb, http_srv, tmp_path):
    """test/news/main.bas completes against the mocked chunked payload."""
    http_srv.chunked = True
    http_srv.domains = json.dumps(
        {
            "items": [{"id": "d1", "label": "Example Domain"}],
            "totalPages": 1,
            "totalItems": 1,
            "page": 1,
            "perPage": 10,
        }
    ).encode()
    article = {
        "domainLabel": "Example Domain",
        "author": "Ada",
        "description": "Short description",
        "keywords": "news",
        "summary": "Summary",
        "sntiment": "neutral",
        "updated": "2026-11-13",
        # Unknown keys are ignored by the TYPE mapper but make the chunked
        # body pass 16 KB, which is what used to overflow the slab.
        "padding": "D" * 20000,
    }
    http_srv.articles = json.dumps(
        {
            "items": [article],
            "totalPages": 1,
            "totalItems": 1,
            "page": 1,
            "perPage": 10,
        }
    ).encode()

    port = http_srv.server_address[1]
    root = tmp_path / "root"
    news = root / "C" / "news"
    news.mkdir(parents=True)
    for name in ("main.bas", "types.bas", "api_calls.bas"):
        shutil.copy(os.path.join(REPO, "test", "news", name), news / name)
    api = news / "api_calls.bas"
    api.write_text(
        api.read_text().replace(
            'base = "https://newsfeeds.co.nz"',
            'base = "http://127.0.0.1:%d"' % port,
        )
    )

    proc = _run(mmb, 'RUN "C:/news/main.bas"\n', root)
    out = proc.stdout + proc.stderr
    if "network not available" in out:
        pytest.skip("host reports no network lease")
    assert proc.returncode == 0, out[-800:]
    assert "Example Domain" in out, out[-800:]
    assert "Articles: 1" in out, out[-800:]
