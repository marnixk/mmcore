"""HTTPS from the packaged macOS app bundle (#1480).

The universal app links a static OpenSSL per slice, so both the arm64 and
x86_64 executables must reach an HTTPS server with verification on. This test
is opt-in: it only runs on macOS with a package built by
``scripts/package-macos-app.sh`` (default ``dist/mmcore.app``, or set
``MMCORE_MACOS_APP`` to the bundle/executable). Ordinary host and QEMU runs
skip it.

Each architecture present in the bundle is driven through a loopback HTTPS
server with a throwaway CA (``WEB TLS CA``), plus rejection cases for an
unknown CA and a hostname mismatch. A slice that cannot execute on this host
(Rosetta missing for x86_64) is skipped for that architecture.
"""
import http.server
import os
import platform
import shutil
import ssl
import subprocess
import sys
import threading

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_APP = os.path.join(REPO, "dist", "mmcore.app")

pytestmark = pytest.mark.skipif(
    sys.platform != "darwin",
    reason="packaged macOS bundle check (darwin only)",
)

ARCHES = ["arm64", "x86_64"]


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b"HELLO"
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass

    def log_message(self, *args):
        pass


@pytest.fixture(scope="module")
def https_srv(tmp_path_factory):
    if shutil.which("openssl") is None:
        pytest.skip("openssl CLI not available for a TLS test certificate")
    tmp = tmp_path_factory.mktemp("macos-tls")
    cert = tmp / "cert.pem"
    key = tmp / "key.pem"
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
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
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


def _app_exe():
    raw = os.environ.get("MMCORE_MACOS_APP") or DEFAULT_APP
    if os.path.isdir(raw) or raw.endswith(".app"):
        exe = os.path.join(raw, "Contents", "MacOS", "mmcore")
    else:
        exe = raw
    if not os.access(exe, os.X_OK):
        pytest.skip(
            "no packaged app at %s (run scripts/package-macos-app.sh or set "
            "MMCORE_MACOS_APP)" % exe
        )
    return exe


def _bundle_arches(exe):
    out = subprocess.run(
        ["lipo", "-archs", exe], check=True, text=True, capture_output=True
    )
    return out.stdout.split()


def _can_run(arch):
    if arch == platform.machine():
        return True
    probe = subprocess.run(
        ["arch", "-" + arch, "/usr/bin/true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return probe.returncode == 0


def _run(exe, program, root, arch):
    env = dict(
        os.environ, MMB_DRIVE_ROOT=str(root), SDL_VIDEODRIVER="dummy"
    )
    if arch == platform.machine():
        cmd = [exe]
    else:
        cmd = ["arch", "-" + arch, exe]
    proc = subprocess.run(
        cmd,
        input=program + "\n",
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    return proc.stdout + proc.stderr


def _ca_program(cert, root, url):
    (root / "C").mkdir(parents=True, exist_ok=True)
    (root / "C" / "CA.PEM").write_bytes(cert.read_bytes())
    return "\n".join(
        [
            'WEB TLS CA "C:/CA.PEM"',
            'r$ = WEB$(HTTP GET "%s")' % url,
            'PRINT "BODY="; r$',
            'PRINT "STATUS="; WEBSTATUS',
            'PRINT "ALIVE="; 6*7',
        ]
    )


@pytest.mark.parametrize("arch", ARCHES)
def test_packaged_https_verified(arch, https_srv, tmp_path):
    exe = _app_exe()
    if arch not in _bundle_arches(exe):
        pytest.skip("%s slice not in %s" % (arch, exe))
    if not _can_run(arch):
        pytest.skip("cannot execute %s here (Rosetta unavailable)" % arch)
    srv, cert = https_srv
    url = "https://localhost:%d/text" % srv.server_address[1]
    out = _run(exe, _ca_program(cert, tmp_path, url), tmp_path, arch)
    assert "TLS unavailable" not in out, out[-800:]
    assert "?WEB" not in out, out[-800:]
    assert "BODY=HELLO" in out, out[-800:]
    assert "STATUS=200" in out, out[-800:]


@pytest.mark.parametrize("arch", ARCHES)
def test_packaged_https_rejects_unknown_ca(arch, https_srv, tmp_path):
    exe = _app_exe()
    if arch not in _bundle_arches(exe):
        pytest.skip("%s slice not in %s" % (arch, exe))
    if not _can_run(arch):
        pytest.skip("cannot execute %s here (Rosetta unavailable)" % arch)
    srv, _cert = https_srv
    url = "https://localhost:%d/text" % srv.server_address[1]
    program = "\n".join(
        [
            "WEB TLS VERIFY",
            'r$ = WEB$(HTTP GET "%s")' % url,
            'PRINT "AFTER-REJECT"',
            'PRINT "ALIVE="; 6*7',
        ]
    )
    out = _run(exe, program, tmp_path, arch)
    assert "BODY=HELLO" not in out, out[-800:]
    assert "certificate verify failed" in out.lower(), out[-800:]
    assert "AFTER-REJECT" in out and "42" in out, out[-800:]


@pytest.mark.parametrize("arch", ARCHES)
def test_packaged_https_rejects_hostname_mismatch(arch, https_srv, tmp_path):
    exe = _app_exe()
    if arch not in _bundle_arches(exe):
        pytest.skip("%s slice not in %s" % (arch, exe))
    if not _can_run(arch):
        pytest.skip("cannot execute %s here (Rosetta unavailable)" % arch)
    srv, cert = https_srv
    url = "https://127.0.0.1:%d/text" % srv.server_address[1]
    out = _run(exe, _ca_program(cert, tmp_path, url), tmp_path, arch)
    assert "BODY=HELLO" not in out, out[-800:]
    assert "?WEB" in out, out[-800:]
    assert "ALIVE=" in out and "42" in out, out[-800:]
