"""WEB: high-level HTTP(S) plus PicoMite-style low-level TCP/TLS (#1475).

These tests drive the native stdio build against hermetic loopback servers, so
they need no QEMU and no external network. HTTPS cases generate a throwaway
self-signed certificate and exercise verified (WEB TLS CA), rejected (no CA)
and explicit insecure (WEB TLS NOVERIFY) modes. They skip when the native
build has no TLS stack (OpenSSL missing) or openssl cannot make a cert.
"""
import http.server
import json
import os
import random
import re
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time

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


def numbered(lines):
    """Turn statements into numbered program lines followed by RUN."""
    out = ["%d %s" % (10 * (i + 1), line) for i, line in enumerate(lines)]
    out.append("RUN")
    return out


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

    def _record(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        self.server.records.append(
            {
                "method": self.command,
                "path": self.path,
                "headers": dict(self.headers.items()),
                "body": body,
            }
        )
        return body

    def _send(self, code, body, ctype="text/plain"):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        self._record()
        if self.path.startswith("/text"):
            self._send(200, b"HELLO")
        elif self.path.startswith("/person"):
            self._send(200, b'{"name":"Ada","age":42}', "application/json")
        elif self.path.startswith("/big"):
            self._send(200, b"A" * 3000)
        elif self.path.startswith("/blob"):
            body = getattr(self.server, "blob", b"")
            if getattr(self.server, "blob_chunked", False):
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    # One chunk keeps the client's rebuild-on-recv decoder
                    # linear even for a 10 MiB body.
                    self.wfile.write(b"%x\r\n%s\r\n0\r\n\r\n"
                                     % (len(body), body))
                except OSError:
                    pass
            else:
                self._send(200, body, "application/json")
        elif self.path.startswith("/malformed"):
            self._send(200, b"{not json", "application/json")
        elif self.path.startswith("/missing"):
            self._send(404, b"missing")
        elif self.path.startswith("/redirect"):
            self.send_response(302)
            self.send_header("Location", "/text")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path.startswith("/chunked"):
            try:
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for part in (b"HELLO", b" WORLD"):
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.write(b"0\r\n\r\n")
            except OSError:
                pass
        elif self.path.startswith("/slow"):
            time.sleep(2.0)
            self._send(200, b"LATE")
        elif self.path.startswith("/echo"):
            self._send(
                200,
                b'{"method":"GET","body":""}',
                "application/json",
            )
        else:
            self._send(200, b"OK")

    def _echo(self):
        body = self._record()
        payload = {
            "method": self.command,
            "body": body.decode("latin-1"),
        }
        self._send(
            200,
            json.dumps(payload, separators=(",", ":")).encode(),
            "application/json",
        )

    do_POST = _echo
    do_PUT = _echo
    do_PATCH = _echo
    do_DELETE = _echo

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


def _make_cert(tmp_path):
    if shutil.which("openssl") is None:
        pytest.skip("openssl CLI not available for a TLS test certificate")
    cert = tmp_path / "cert.pem"
    key = tmp_path / "key.pem"
    proc = subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(cert), "-days", "1",
            "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost",
        ],
        capture_output=True,
    )
    if proc.returncode != 0 or not cert.is_file():
        pytest.skip("could not generate a TLS test certificate")
    return cert, key


@pytest.fixture()
def https_srv(tmp_path):
    cert, key = _make_cert(tmp_path)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    srv.records = []
    srv.daemon_threads = True
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(cert), str(key))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv, cert
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=5)


def _tls_available(mmb_bin, srv, tmp_path):
    out = run(
        mmb_bin,
        'WEB OPEN TLS CLIENT "localhost",%d, 2000\n' % _port(srv),
        tmp_path,
    )
    if "TLS unavailable" in out:
        pytest.skip("native build has no TLS stack (OpenSSL not found)")
    return True


# ---- high-level text forms -------------------------------------------


def test_text_verbs_and_status(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            'g$ = WEB$(HTTP GET "http://127.0.0.1:%d/echo")' % port,
            'd$ = WEB$(HTTP DELETE "http://127.0.0.1:%d/echo")' % port,
            'p$ = WEB$(HTTP POST "http://127.0.0.1:%d/echo", "one")' % port,
            'u$ = WEB$(HTTP PUT "http://127.0.0.1:%d/echo", "two")' % port,
            'h$ = WEB$(HTTP PATCH "http://127.0.0.1:%d/echo", "three")' % port,
            'PRINT "GET:"; g$',
            'PRINT "DEL:"; d$',
            'PRINT "POST:"; p$',
            'PRINT "PUT:"; u$',
            'PRINT "PATCH:"; h$',
            'PRINT "STATUS:"; WEBSTATUS',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert '"method":"GET"' in out
    assert '"method":"DELETE"' in out
    assert '"method":"POST"' in out and '"body":"one"' in out
    assert '"method":"PUT"' in out and '"body":"two"' in out
    assert '"method":"PATCH"' in out and '"body":"three"' in out
    assert "STATUS:200" in out
    sent = http_srv.records
    assert [r["method"] for r in sent] == ["GET", "DELETE", "POST", "PUT", "PATCH"]


def test_url_query_and_default_path(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            'a$ = WEB$(HTTP GET "http://127.0.0.1:%d/text?x=1")' % port,
            'b$ = WEB$(HTTP GET "http://127.0.0.1:%d/")' % port,
            'PRINT "A="; a$',
            'PRINT "B="; b$',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "A=HELLO" in out
    assert "B=OK" in out
    assert http_srv.records[0]["path"] == "/text?x=1"
    assert http_srv.records[1]["path"] == "/"


def test_status_and_response_headers(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/text")' % port,
            'PRINT "BODY="; r$',
            'PRINT "STATUS="; WEBSTATUS',
            'PRINT "CT="; WEBHEADER$("content-type")',
            'PRINT "MISS="; WEBHEADER$("X-Nope")',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "BODY=HELLO" in out
    assert "STATUS=200" in out
    assert "CT=text/plain" in out
    assert "MISS=" in out


def test_custom_headers_bounds_and_precedence(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "OPTION BASE 1",
            "DIM h$(1)",
            'h$(1) = "X-One: 1"',
            'a$ = WEB$(HTTP GET "http://127.0.0.1:%d/echo" HEADERS h$())' % port,
            'PRINT "ONE:"; a$',
        ]
        + numbered(
            [
                "TYPE P",
                "name AS STRING * 16",
                "END TYPE",
                "OPTION BASE 0",
                "DIM g$(1)",
                'g$(0) = "Accept: application/json"',
                'g$(1) = "Content-Type: text/plain"',
                "DIM p AS P",
                'p.name = "Ada"',
                'b$ = WEB$(HTTP POST "http://127.0.0.1:%d/echo", p HEADERS g$())'
                % port,
                'PRINT "TWO:"; b$',
            ]
        )
    )
    out = run(mmb, program, tmp_path)
    assert "ONE:" in out and '"method":"GET"' in out
    first, second = http_srv.records[0], http_srv.records[1]
    assert first["headers"].get("X-One") == "1"
    assert second["headers"].get("Accept") == "application/json"
    # custom Content-Type overrides the generated application/json
    assert second["headers"].get("Content-Type") == "text/plain"
    assert '"name":"ada"' in second["body"].decode().lower()


def test_header_crlf_and_content_length_rejected(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "DIM h$(1)",
            'h$(0) = "X-Bad: a" + CHR$(13) + "X-Inject: b"',
            'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/text" HEADERS h$())' % port,
            'PRINT "AFTER-CRLF"',
            'h$(0) = "Content-Length: 5"',
            'r$ = WEB$(HTTP POST "http://127.0.0.1:%d/text", "abc" HEADERS h$())'
            % port,
            'PRINT "AFTER-CL"',
            "PRINT 6*7",
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "?WEB: CR/LF in header" in out
    assert "AFTER-CRLF" in out
    assert "?WEB: custom Content-Length not allowed" in out
    assert "AFTER-CL" in out
    assert "42" in out


def test_large_body_and_request(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            'big$ = STRING$(2000, "A")',
            'r$ = WEB$(HTTP POST "http://127.0.0.1:%d/echo", big$)' % port,
            'PRINT "ECHO-LEN:"; LEN(r$)',
            'g$ = WEB$(HTTP GET "http://127.0.0.1:%d/big")' % port,
            'PRINT "GET-LEN:"; LEN(g$)',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert http_srv.records[0]["body"] == b"A" * 2000
    assert "GET-LEN:3000" in out


@pytest.mark.parametrize("chunked", [False, True], ids=["content-length", "chunked"])
@pytest.mark.parametrize(
    "size",
    [20000, 65536, 1024 * 1024, 10 * 1024 * 1024],
    ids=["20k", "64k", "1m", "10m"],
)
def test_large_response_bodies(mmb, http_srv, tmp_path, chunked, size):
    """Bodies at and above every pool boundary, both framing styles."""
    http_srv.blob = b"Z" * size
    http_srv.blob_chunked = chunked
    port = _port(http_srv)
    out = run(
        mmb,
        'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/blob")\n'
        'PRINT "LEN:"; LEN(r$)\n' % port,
        tmp_path,
        timeout=180,
    )
    assert "LEN:%d" % size in out, out[-800:]


def test_non_2xx_body_is_available(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    out = run(
        mmb,
        'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/missing")\n'
        'PRINT "BODY="; r$\n'
        'PRINT "STATUS="; WEBSTATUS\n' % port,
        tmp_path,
    )
    assert "BODY=missing" in out
    assert "STATUS=404" in out


def test_chunked_and_redirect(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            'c$ = WEB$(HTTP GET "http://127.0.0.1:%d/chunked")' % port,
            'PRINT "CHUNK="; c$',
            'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/redirect")' % port,
            'PRINT "REDIR="; r$',
            'PRINT "RSTATUS="; WEBSTATUS',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "CHUNK=HELLO WORLD" in out
    assert "REDIR=HELLO" in out
    assert "RSTATUS=200" in out


# ---- TYPE payload / response ----------------------------------------


def test_typed_request_and_response(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        numbered(
            [
                "TYPE Person",
                "name AS STRING * 32",
                "age AS INTEGER",
                "END TYPE",
                "DIM postPerson AS Person",
                'postPerson.name = "Ada"',
                "postPerson.age = 42",
                'e$ = WEB$(HTTP POST "http://127.0.0.1:%d/echo", postPerson)'
                % port,
                'PRINT "ECHO:"; e$',
                "DIM gotPerson AS Person",
                'gotPerson = WEB(HTTP GET "http://127.0.0.1:%d/person" AS Person)'
                % port,
                'PRINT "NAME:"; gotPerson.name',
                'PRINT "AGE:"; gotPerson.age',
            ]
        )
    )
    out = run(mmb, program, tmp_path)
    assert http_srv.records[0]["body"] == b'{"NAME":"Ada","AGE":42}'
    # TYPE payload defaults to JSON
    assert http_srv.records[0]["headers"].get("Content-Type") == "application/json"
    assert "NAME:Ada" in out
    assert "AGE:42" in out


def test_malformed_json_errors(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        numbered(
            [
                "TYPE P",
                "name AS STRING * 8",
                "END TYPE",
                "DIM p AS P",
                'p = WEB(HTTP GET "http://127.0.0.1:%d/malformed" AS P)' % port,
            ]
        )
    )
    out = run(mmb, program, tmp_path)
    assert "?JSON" in out


def test_typed_assignment_mismatch(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        numbered(
            [
                "TYPE P",
                "name AS STRING * 8",
                "END TYPE",
                "TYPE Q",
                "x AS INTEGER",
                "END TYPE",
                "DIM q AS Q",
                'q = WEB(HTTP GET "http://127.0.0.1:%d/person" AS P)' % port,
            ]
        )
    )
    out = run(mmb, program, tmp_path)
    assert "?TYPE MISMATCH" in out


def test_timeout_keeps_interpreter_usable(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "WEB TIMEOUT 400",
            'r$ = WEB$(HTTP GET "http://127.0.0.1:%d/slow")' % port,
            'PRINT "AFTER-TIMEOUT"',
            "PRINT 7*6",
        ]
    )
    out = run(mmb, program, tmp_path, timeout=30)
    assert "?WEB: response timeout" in out
    assert "AFTER-TIMEOUT" in out
    assert "42" in out


# ---- URL file loads (#1505) ------------------------------------------

ASSET_DIR = os.path.join(REPO, "ramdisk", "tests")


def _asset(name):
    with open(os.path.join(ASSET_DIR, name), "rb") as fh:
        return fh.read()


def _make_png(width, height, rgb):
    import struct
    import zlib

    row = bytes(rgb) * width
    raw = b"".join(b"\x00" + row for _ in range(height))

    def chunk(typ, data):
        return (
            struct.pack(">I", len(data))
            + typ
            + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 0))
        + chunk(b"IEND", b"")
    )


def _pixel(out, label="PIX"):
    m = re.search(label + r":\s*(\d+)", out)
    assert m, out[-800:]
    return int(m.group(1))


def test_load_png_from_http_url(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    port = _port(http_srv)
    out = run(
        mmb,
        'MODE 1\nCLS\nLOAD PNG "http://127.0.0.1:%d/blob"\n'
        'PRINT "PIX:"; PIXEL(0,0)\n' % port,
        tmp_path,
    )
    assert "?PNG" not in out and "?WEB" not in out, out[-800:]
    assert ((_pixel(out) >> 16) & 255) > 150
    assert http_srv.records[0]["path"] == "/blob"


def test_load_png_url_headers(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    port = _port(http_srv)
    program = "\n".join(
        [
            "DIM h$(2)",
            'h$(0) = "X-Token: secret"',
            'h$(1) = "Accept: image/png"',
            "MODE 1",
            'LOAD PNG "http://127.0.0.1:%d/blob" HEADERS h$()' % port,
            'PRINT "OK"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "?WEB" not in out and "?PNG" not in out, out[-800:]
    assert "OK" in out
    rec = http_srv.records[0]
    assert rec["headers"].get("X-Token") == "secret"
    assert rec["headers"].get("Accept") == "image/png"


def test_load_jpg_from_http_url(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.JPG")
    port = _port(http_srv)
    out = run(
        mmb,
        'MODE 1\nCLS\nLOAD JPG "http://127.0.0.1:%d/blob"\n'
        'PRINT "PIX:"; PIXEL(1,1)\n' % port,
        tmp_path,
    )
    assert "?JPEG" not in out and "?WEB" not in out, out[-800:]
    assert ((_pixel(out) >> 16) & 255) > 60


def test_load_png_url_404_is_catchable(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "10 ON ERROR GOTO 80",
            "20 MODE 1",
            '30 LOAD PNG "http://127.0.0.1:%d/missing"' % port,
            '40 PRINT "NOTREACHED"',
            "50 END",
            '80 PRINT "ERR:"; MM.ERRMSG$',
            '90 PRINT "ALIVE:"; 6*7',
            "RUN",
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "NOTREACHED" not in out
    assert "404" in out, out[-800:]
    assert "ALIVE: 42" in out or "ALIVE:42" in out


def test_load_png_url_timeout_is_catchable(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        [
            "10 ON ERROR GOTO 80",
            "20 WEB TIMEOUT 300",
            "30 MODE 1",
            '40 LOAD PNG "http://127.0.0.1:%d/slow"' % port,
            '50 PRINT "NOTREACHED"',
            "60 END",
            '80 PRINT "ERR:"; MM.ERRMSG$',
            '90 PRINT "ALIVE:"; 6*7',
            "RUN",
        ]
    )
    out = run(mmb, program, tmp_path, timeout=30)
    assert "NOTREACHED" not in out
    assert "timeout" in out.lower(), out[-800:]
    assert "ALIVE: 42" in out or "ALIVE:42" in out


def test_load_png_url_over_one_megabyte(mmb, http_srv, tmp_path):
    width, height = 900, 700
    noise = random.Random(1505).randbytes(width * height * 3)
    rows = b"".join(
        b"\x00" + noise[y * width * 3:(y + 1) * width * 3]
        for y in range(height)
    )
    import struct
    import zlib

    def chunk(typ, data):
        return (
            struct.pack(">I", len(data))
            + typ
            + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(rows, 0))
        + chunk(b"IEND", b"")
    )
    assert len(png) > 1024 * 1024
    http_srv.blob = png
    port = _port(http_srv)
    out = run(
        mmb,
        'MODE 1\nCLS\nLOAD PNG "http://127.0.0.1:%d/blob"\n'
        'PRINT "PIX:"; PIXEL(3,3)\n' % port,
        tmp_path,
        timeout=120,
    )
    assert "?PNG" not in out and "?WEB" not in out, out[-800:]
    assert _pixel(out) >= 0


# ---- in-memory session cache (#1507) --------------------------------


def _blob_url(srv):
    return "http://127.0.0.1:%d/blob" % _port(srv)


def _load_url_prog(srv, n=2, prefix=()):
    url = _blob_url(srv)
    lines = list(prefix) + ["MODE 1"]
    lines += ['LOAD PNG "%s"' % url for _ in range(n)]
    lines.append('PRINT "DONE"')
    return "\n".join(lines)


def test_web_cache_off_by_default_refetches(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    out = run(mmb, _load_url_prog(http_srv, 2), tmp_path)
    assert "DONE" in out and "?WEB" not in out and "?PNG" not in out
    assert len(http_srv.records) == 2


def test_web_cache_on_avoids_second_request(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    out = run(
        mmb,
        _load_url_prog(http_srv, 2, prefix=["OPTION WEB CACHE ON"]),
        tmp_path,
    )
    assert "DONE" in out and "?WEB" not in out and "?PNG" not in out
    assert len(http_srv.records) == 1


def test_web_cache_headers_make_separate_entries(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    url = _blob_url(http_srv)
    program = "\n".join(
        [
            "OPTION WEB CACHE ON",
            "MODE 1",
            "DIM h$(1)",
            'h$(0) = "X-Token: a"',
            'LOAD PNG "%s" HEADERS h$()' % url,
            'LOAD PNG "%s" HEADERS h$()' % url,
            'h$(0) = "X-Token: b"',
            'LOAD PNG "%s" HEADERS h$()' % url,
            'LOAD PNG "%s"' % url,
            'PRINT "DONE"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "DONE" in out and "?WEB" not in out and "?PNG" not in out
    assert [r["headers"].get("X-Token") for r in http_srv.records] == [
        "a",
        "b",
        None,
    ]


def test_web_cache_clear_forces_refetch(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    url = _blob_url(http_srv)
    program = "\n".join(
        [
            "OPTION WEB CACHE ON",
            "MODE 1",
            'LOAD PNG "%s"' % url,
            'LOAD PNG "%s"' % url,
            "WEB CACHE CLEAR",
            'LOAD PNG "%s"' % url,
            'PRINT "DONE"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "DONE" in out and "?WEB" not in out and "?PNG" not in out
    assert len(http_srv.records) == 2


def test_web_cache_run_forces_refetch(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    url = _blob_url(http_srv)
    program = "\n".join(
        [
            "10 OPTION WEB CACHE ON",
            "20 MODE 1",
            '30 LOAD PNG "%s"' % url,
            '40 PRINT "RUN-DONE"',
            "RUN",
            "RUN",
        ]
    )
    out = run(mmb, program, tmp_path)
    assert out.count("RUN-DONE") == 2, out[-800:]
    assert "?WEB" not in out and "?PNG" not in out
    assert len(http_srv.records) == 2


def test_web_cache_off_clears_and_refetches(mmb, http_srv, tmp_path):
    http_srv.blob = _asset("TEST.PNG")
    url = _blob_url(http_srv)
    program = "\n".join(
        [
            "OPTION WEB CACHE ON",
            "MODE 1",
            'LOAD PNG "%s"' % url,
            "OPTION WEB CACHE OFF",
            'LOAD PNG "%s"' % url,
            'LOAD PNG "%s"' % url,
            'PRINT "DONE"',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "DONE" in out and "?WEB" not in out and "?PNG" not in out
    assert len(http_srv.records) == 3


def test_web_cache_web_get_only_and_non_get(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    base = "http://127.0.0.1:%d" % port
    program = "\n".join(
        [
            "OPTION WEB CACHE ON",
            'a$ = WEB$(HTTP GET "%s/text")' % base,
            'b$ = WEB$(HTTP GET "%s/text")' % base,
            'PRINT "STATUS="; WEBSTATUS',
            'c$ = WEB$(HTTP DELETE "%s/echo")' % base,
            'd$ = WEB$(HTTP DELETE "%s/echo")' % base,
            'e$ = WEB$(HTTP POST "%s/echo", "x")' % base,
            'f$ = WEB$(HTTP POST "%s/echo", "x")' % base,
            'PRINT "A="; a$; " B="; b$',
        ]
    )
    out = run(mmb, program, tmp_path)
    assert "A=HELLO B=HELLO" in out
    assert "STATUS=200" in out
    assert [r["method"] for r in http_srv.records] == [
        "GET",
        "DELETE",
        "DELETE",
        "POST",
        "POST",
    ]
    assert http_srv.records[0]["path"] == "/text"


def test_web_cache_typed_get(mmb, http_srv, tmp_path):
    port = _port(http_srv)
    program = "\n".join(
        numbered(
            [
                "OPTION WEB CACHE ON",
                "TYPE P",
                "name AS STRING * 32",
                "age AS INTEGER",
                "END TYPE",
                "DIM p AS P",
                'p = WEB(HTTP GET "http://127.0.0.1:%d/person" AS P)' % port,
                'p = WEB(HTTP GET "http://127.0.0.1:%d/person" AS P)' % port,
                'PRINT "NAME="; p.name',
                'PRINT "AGE="; p.age',
            ]
        )
    )
    out = run(mmb, program, tmp_path)
    assert "NAME=Ada" in out
    assert "AGE=42" in out
    assert len(http_srv.records) == 1


def test_load_png_from_https_url(mmb, https_srv, tmp_path):
    srv, _cert = https_srv
    _tls_available(mmb, srv, tmp_path)
    srv.blob = _asset("TEST.PNG")
    port = _port(srv)
    out = run(
        mmb,
        'WEB TLS NOVERIFY\nMODE 1\nCLS\n'
        'LOAD PNG "https://localhost:%d/blob"\n'
        'PRINT "PIX:"; PIXEL(0,0)\n' % port,
        tmp_path,
    )
    assert "?PNG" not in out and "?WEB" not in out, out[-800:]
    assert ((_pixel(out) >> 16) & 255) > 150


# ---- HTTPS -----------------------------------------------------------


def test_https_verified_and_noverify(mmb, https_srv, tmp_path):
    srv, cert = https_srv
    _tls_available(mmb, srv, tmp_path)
    port = _port(srv)
    root = tmp_path / "root"
    (root / "C").mkdir(parents=True)
    (root / "C" / "CA.PEM").write_bytes(cert.read_bytes())

    verified = "\n".join(
        [
            'WEB TLS CA "C:/CA.PEM"',
            'r$ = WEB$(HTTP GET "https://localhost:%d/text")' % port,
            'PRINT "BODY="; r$',
            'PRINT "STATUS="; WEBSTATUS',
        ]
    )
    out = run(mmb, verified, root)
    assert "BODY=HELLO" in out
    assert "STATUS=200" in out

    rejected = "\n".join(
        [
            "WEB TLS VERIFY",
            'r$ = WEB$(HTTP GET "https://localhost:%d/text")' % port,
            'PRINT "AFTER-REJECT"',
        ]
    )
    out = run(mmb, rejected, root)
    assert "certificate verify failed" in out.lower()
    assert "AFTER-REJECT" in out

    insecure = "\n".join(
        [
            "WEB TLS NOVERIFY",
            'r$ = WEB$(HTTP GET "https://localhost:%d/text")' % port,
            'PRINT "BODY="; r$',
        ]
    )
    out = run(mmb, insecure, root)
    assert "BODY=HELLO" in out

    typed = "\n".join(
        numbered(
            [
                "TYPE P",
                "name AS STRING * 32",
                "age AS INTEGER",
                "END TYPE",
                "DIM p AS P",
                'WEB TLS CA "C:/CA.PEM"',
                'p = WEB(HTTP GET "https://localhost:%d/person" AS P)' % port,
                'PRINT "NAME="; p.name',
                'PRINT "AGE="; p.age',
            ]
        )
    )
    out = run(mmb, typed, root)
    assert "NAME=Ada" in out
    assert "AGE=42" in out


def test_low_level_tls_client(mmb, https_srv, tmp_path):
    srv, cert = https_srv
    _tls_available(mmb, srv, tmp_path)
    port = _port(srv)
    root = tmp_path / "root"
    (root / "C").mkdir(parents=True)
    (root / "C" / "CA.PEM").write_bytes(cert.read_bytes())
    program = "\n".join(
        numbered(
            [
                "DIM b%(2048)",
                'WEB TLS CA "C:/CA.PEM"',
                'WEB OPEN TLS CLIENT "localhost",%d, 5000' % port,
                'WEB TCP CLIENT REQUEST "GET /text HTTP/1.0" + CHR$(13) + '
                'CHR$(10) + CHR$(13) + CHR$(10), b%(), 5000',
                'PRINT "LEN="; b%(0)',
                's$ = ""',
                "FOR i = 1 TO b%(0)",
                "s$ = s$ + CHR$(b%(i))",
                "NEXT i",
                'PRINT "RAW="; s$',
                "WEB CLOSE TCP CLIENT",
            ]
        )
    )
    out = run(mmb, program, root)
    assert "LEN=" in out
    assert "HELLO" in out
    assert "?WEB" not in out


# ---- low-level client ------------------------------------------------


def _raw_server(response=b"PONG"):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(2)
    got = []

    def serve():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            try:
                conn.settimeout(2)
                got.append(conn.recv(64))
                conn.sendall(response)
                time.sleep(0.05)
            except OSError:
                pass
            finally:
                conn.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return srv, srv.getsockname()[1], got, thread


def test_low_level_client_lifecycle(mmb, tmp_path):
    srv, port, got, thread = _raw_server()
    try:
        program = "\n".join(
            [
                "DIM b%(256)",
                'WEB OPEN TCP CLIENT "127.0.0.1",%d, 5000' % port,
                'WEB TCP CLIENT REQUEST "PING1", b%(), 5000',
                'PRINT "R1="; b%(0)',
                "WEB CLOSE TCP CLIENT",
                'WEB OPEN TCP CLIENT "127.0.0.1",%d, 5000' % port,
                'WEB TCP CLIENT REQUEST "PING2", b%(), 5000',
                'PRINT "R2="; b%(0)',
                "WEB CLOSE TCP CLIENT",
                'PRINT "AFTER"',
            ]
        )
        out = run(mmb, program, tmp_path)
    finally:
        srv.close()
        thread.join(timeout=3)
    assert "R1=4" in out
    assert "R2=4" in out
    assert "AFTER" in out
    assert got == [b"PING1", b"PING2"]


def test_low_level_client_write_read(mmb, tmp_path):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    got = []

    def serve():
        conn, _ = srv.accept()
        conn.settimeout(3)
        try:
            got.append(conn.recv(64))
            conn.sendall(b"WORLD")
            time.sleep(0.1)
        finally:
            conn.close()
            srv.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    program = "\n".join(
        [
            "DIM w%(4)",
            "w%(0) = 2",
            'w%(1) = ASC("H")',
            'w%(2) = ASC("I")',
            "DIM r%(64)",
            'WEB OPEN TCP CLIENT "127.0.0.1",%d, 5000' % port,
            "WEB TCP CLIENT WRITE w%(), 5000",
            "WEB TCP CLIENT READ r%(), 5000",
            'PRINT "LEN="; r%(0)',
            'PRINT "BYTE="; CHR$(r%(1))',
            "WEB CLOSE TCP CLIENT",
        ]
    )
    out = run(mmb, program, tmp_path)
    thread.join(timeout=5)
    assert "LEN=5" in out
    assert "BYTE=W" in out
    assert got and got[0] == b"HI"


# ---- server ----------------------------------------------------------


def _free_port():
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def _client_roundtrip(port, request=b"GET / HTTP/1.0\r\n\r\n"):
    result = {}

    def client():
        for _ in range(100):
            try:
                c = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.05)
        else:
            result["error"] = "connect failed"
            return
        c.settimeout(6)
        try:
            c.sendall(request)
            data = b""
            while True:
                chunk = c.recv(4096)
                if not chunk:
                    break
                data += chunk
            result["data"] = data
        except OSError as exc:
            result["error"] = repr(exc)
        finally:
            c.close()

    thread = threading.Thread(target=client, daemon=True)
    thread.start()
    return result, thread


def test_server_read_send_close(mmb, tmp_path):
    port = _free_port()
    result, thread = _client_roundtrip(port)
    program = "\n".join(
        [
            "WEB TCP PORT %d" % port,
            "PAUSE 800",
            "WEB TCP INTERRUPT",
            "DIM b%(4096)",
            "WEB TCP READ b%(), 5000",
            'PRINT "REQLEN="; b%(0)',
            "DIM out%(4)",
            "out%(0) = 2",
            'out%(1) = ASC("O")',
            'out%(2) = ASC("K")',
            "WEB TCP SEND out%(), 5000",
            "WEB TCP CLOSE",
            'PRINT "DONE"',
        ]
    )
    out = run(mmb, program, tmp_path)
    thread.join(timeout=8)
    assert "REQLEN=18" in out
    assert "DONE" in out
    assert result.get("data") == b"OK"


def _transmit_once(mmb, tmp_path, body_lines, request=b"GET / HTTP/1.0\r\n\r\n"):
    port = _free_port()
    result, thread = _client_roundtrip(port, request)
    program = "\n".join(
        [
            "WEB TCP PORT %d" % port,
            "PAUSE 800",
            "WEB TCP INTERRUPT",
            "DIM b%(4096)",
            "WEB TCP READ b%(), 5000",
        ]
        + body_lines
        + ["WEB TCP CLOSE"]
    )
    out = run(mmb, program, tmp_path)
    thread.join(timeout=8)
    return out, result.get("data", b"")


def test_server_transmit_code(mmb, tmp_path):
    out, data = _transmit_once(mmb, tmp_path, ["WEB TRANSMIT CODE 0, 404"])
    assert "HTTP/1.1 404" in data.decode()
    assert "Content-Length: 0" in data.decode()


def test_server_transmit_file(mmb, tmp_path):
    root = tmp_path / "root"
    (root / "C").mkdir(parents=True)
    (root / "C" / "BODY.TXT").write_bytes(b"FILE-BODY")
    out, data = _transmit_once(
        mmb,
        root,
        ['WEB TRANSMIT FILE 0, "C:/BODY.TXT", "text/plain"'],
    )
    text = data.decode()
    assert "HTTP/1.1 200" in text
    assert "Content-Type: text/plain" in text
    assert text.endswith("FILE-BODY")


def test_server_transmit_page_substitution(mmb, tmp_path):
    root = tmp_path / "root"
    (root / "C").mkdir(parents=True)
    (root / "C" / "PAGE.HTM").write_text("<h1><% 1+2 %></h1><p><% name$ %></p>")
    out, data = _transmit_once(
        mmb,
        root,
        [
            'name$ = "Ada"',
            'WEB TRANSMIT PAGE 0, "C:/PAGE.HTM"',
        ],
    )
    text = data.decode()
    assert "HTTP/1.1 200" in text
    assert "text/html" in text
    assert "<h1>3</h1>" in text
    assert "<p>Ada</p>" in text
