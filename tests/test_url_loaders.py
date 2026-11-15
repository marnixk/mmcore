"""URL + HEADERS loading for the whole-file loaders (#1506).

Every command here goes through the shared #1505 resolver against a hermetic
loopback HTTP server, so no external network is needed. The native stdio build
is used for speed; the same code paths serve the QEMU/hardware targets.
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


def run(mmb_bin, program, root, timeout=60):
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    proc = subprocess.run(
        [mmb_bin],
        input=program if program.endswith("\n") else program + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )
    return proc.stdout + proc.stderr


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b""
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            body = self.rfile.read(length)
        self.server.records.append(
            {
                "path": self.path,
                "headers": dict(self.headers.items()),
                "body": body,
            }
        )
        if self.path.startswith("/missing"):
            self._send(404, b"missing")
        elif self.path.startswith("/slow"):
            import time

            time.sleep(2.0)
            self._send(200, b"LATE")
        else:
            self._send(200, getattr(self.server, "blob", b""))

    def _send(self, code, body, ctype="application/octet-stream"):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
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
    srv.records = []
    srv.daemon_threads = True
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=5)


def _port(srv):
    return srv.server_address[1]


def _asset(*parts):
    with open(os.path.join(REPO, *parts), "rb") as fh:
        return fh.read()


def test_sprite_loadpng_url_headers(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("ramdisk", "tests", "TEST.PNG")
    port = _port(http_srv)
    program = "\n".join(
        [
            "MODE 1",
            "CLS",
            "DIM h$(1)",
            'h$(0) = "X-Token: sprite"',
            'SPRITE LOADPNG 1, "http://127.0.0.1:%d/blob" HEADERS h$()' % port,
            "SPRITE SHOW 1, 0, 0",
            'PRINT "PIX:"; PIXEL(1,1)',
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "?PNG" not in out and "?WEB" not in out, out[-800:]
    assert "OK" in out
    assert http_srv.records[0]["headers"].get("X-Token") == "sprite"
    assert int(out.split("PIX:")[1].split()[0]) >= 0


def test_tdf_load_url(mmb, http_srv, tmp_path):
    http_srv.blob = _asset(
        "ramdisk", "fonts", "tdf", "mono", "STANDARD.TDF"
    )
    port = _port(http_srv)
    program = "\n".join(
        [
            'TDF LOAD "http://127.0.0.1:%d/blob"' % port,
            "PRINT TDF.NAME$",
            'TDF PRINT 0, 0, "HI"',
            "TDF CLOSE 0",
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "TDF:" not in out and "?WEB" not in out, out[-800:]
    assert "STANDARD" in out.upper()
    assert "OK" in out


def test_play_wav_url_headers(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("ramdisk", "tests", "TEST.WAV")
    port = _port(http_srv)
    program = "\n".join(
        [
            "DIM h$(1)",
            'h$(0) = "X-Token: wav"',
            'PLAY WAV "http://127.0.0.1:%d/blob" HEADERS h$()' % port,
            "PLAY STOP",
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "?WAV" not in out and "?WEB" not in out, out[-800:]
    assert "OK" in out
    assert http_srv.records[0]["headers"].get("X-Token") == "wav"


def test_play_mod_url(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("ramdisk", "tests", "TEST.MOD")
    port = _port(http_srv)
    out = run(
        mmb,
        'PLAY MOD "http://127.0.0.1:%d/blob"\nPLAY STOP\nPRINT "OK"\n'
        % port,
        tmp_path,
    )
    assert "?WEB" not in out, out[-800:]
    assert "OK" in out


@pytest.mark.parametrize(
    "command,asset",
    [("MP3", "TEST.MP3"), ("XM", "TEST.XM"), ("S3M", "TEST.S3M")],
)
def test_play_other_url_formats(mmb, http_srv, tmp_path, command, asset):
    http_srv.blob = _asset("ramdisk", "tests", asset)
    port = _port(http_srv)
    out = run(
        mmb,
        'PLAY %s "http://127.0.0.1:%d/blob"\nPLAY STOP\nPRINT "OK"\n'
        % (command, port),
        tmp_path,
    )
    assert "?WEB" not in out and "?" + command not in out, out[-800:]
    assert "OK" in out


def test_play_sfx_url(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("ramdisk", "tests", "TEST.WAV")
    port = _port(http_srv)
    out = run(
        mmb,
        'PLAY SFX "http://127.0.0.1:%d/blob", 50\nPRINT "OK"\n' % port,
        tmp_path,
    )
    assert "?WEB" not in out, out[-800:]
    assert "OK" in out


def test_open_url_for_input_headers(mmb, http_srv, tmp_path):
    http_srv.blob = b"LINE-ONE\nLINE-TWO\n"
    port = _port(http_srv)
    program = "\n".join(
        [
            "DIM h$(1)",
            'h$(0) = "X-Token: open"',
            'OPEN "http://127.0.0.1:%d/blob" FOR INPUT HEADERS h$() AS #1'
            % port,
            "LINE INPUT #1, a$",
            "PRINT a$",
            "CLOSE #1",
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "?WEB" not in out and "?FILE" not in out, out[-800:]
    assert "LINE-ONE" in out
    assert "OK" in out
    assert http_srv.records[0]["headers"].get("X-Token") == "open"


def test_open_url_is_read_only(mmb, http_srv, tmp_path):
    http_srv.blob = b"abc"
    port = _port(http_srv)
    program = "\n".join(
        [
            "10 ON ERROR GOTO 80",
            '20 OPEN "http://127.0.0.1:%d/blob" FOR OUTPUT AS #1' % port,
            '30 PRINT "NOTREACHED"',
            "40 END",
            '80 PRINT "ERR:"; MM.ERRMSG$',
            '90 PRINT "ALIVE:"; 6*7',
            "RUN",
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "NOTREACHED" not in out
    assert "READ ONLY" in out.upper(), out[-800:]
    assert "ALIVE: 42" in out or "ALIVE:42" in out


def test_open_url_404_is_catchable(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "10 ON ERROR GOTO 80",
            '20 OPEN "http://127.0.0.1:%d/missing" FOR INPUT AS #1' % port,
            '30 PRINT "NOTREACHED"',
            "40 END",
            '80 PRINT "ERR:"; MM.ERRMSG$',
            '90 PRINT "ALIVE:"; 6*7',
            "RUN",
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "NOTREACHED" not in out
    assert "404" in out, out[-800:]
    assert "ALIVE: 42" in out or "ALIVE:42" in out


def test_open_url_large_payload(mmb, http_srv, tmp_path):
    payload = b"0123456789ABCDEF\n" * 100000
    assert len(payload) > 1024 * 1024
    http_srv.blob = payload
    port = _port(http_srv)
    program = "\n".join(
        [
            'OPEN "http://127.0.0.1:%d/blob" FOR INPUT AS #1' % port,
            'PRINT "LEN:"; LOF(#1)',
            "LINE INPUT #1, a$",
            "PRINT a$",
            "CLOSE #1",
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path, timeout=120)
    assert "?WEB" not in out and "?FILE" not in out, out[-800:]
    assert "LEN:%d" % len(payload) in out
    assert "0123456789ABCDEF" in out
    assert "OK" in out
