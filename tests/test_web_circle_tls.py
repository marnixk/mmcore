"""HTTPS on the bare-metal Circle build (#1479).

The native suite (``test_web.py``) already covers WEB's HTTP/TLS logic against
a host OpenSSL backend. This module exercises the Circle TLS seam end to end:
QEMU's usb-net guest reaches a throwaway host HTTPS server at ``10.0.2.2``,
installs that server's self-signed CA with ``WEB TLS CA`` and checks verified,
rejected and explicit insecure (``WEB TLS NOVERIFY``) modes.

Guest -> host TCP through SLIRP is opt-in and transiently usable, so the module
is skipped unless ``MMCORE_LIVE_NET=1`` (same gate as the other live tests).
"""

import http.server
import os
import shutil
import ssl
import subprocess
import threading

import pytest

from net_util import live_net_enabled

pytestmark = pytest.mark.skipif(
    not live_net_enabled(),
    reason="set MMCORE_LIVE_NET=1 to run live guest<->host TLS tests",
)

HOST = "10.0.2.2"


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b"CIRCLE-TLS-OK"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def _make_cert(tmp_path):
    if shutil.which("openssl") is None:
        pytest.skip("openssl CLI not available for a TLS test certificate")
    cert = tmp_path / "cert.pem"
    key = tmp_path / "key.pem"
    proc = subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(cert), "-days", "1",
            "-subj", "/CN=10.0.2.2",
            # mbedTLS 2.28 matches only dNSName SANs (it has no IP-address
            # SAN matcher), so the throwaway cert carries the guest's host
            # literal as a DNS SAN.
            "-addext", "subjectAltName=DNS:10.0.2.2",
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


def _install_ca(con, cert):
    lines = cert.read_text().splitlines()
    assert con.send_line('OPEN "A:/CA.PEM" FOR OUTPUT AS #1') == ""
    for line in lines:
        assert con.send_line('PRINT #1, "%s"' % line) == ""
    assert con.send_line("CLOSE #1") == ""


def _ethernet_up(con):
    from test_qemu_ethernet import _wait_dhcp

    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = con.send_line("OPTION ETHERNET ON", timeout=25)
    if "10.0.2." not in out:
        out += "\n" + _wait_dhcp(con)
    assert "10.0.2." in out, out


def test_circle_https_verified_and_noverify(net_console, https_srv, tmp_path):
    from net_util import require_host_tcp

    con = net_console
    srv, cert = https_srv
    _ethernet_up(con)
    require_host_tcp(con)
    port = srv.server_address[1]
    url = "https://%s:%d/" % (HOST, port)

    # No CA installed and the server is not in the embedded Mozilla bundle:
    # verification must reject it instead of returning the body.
    out = con.send_line('PRINT WEB$(HTTP GET "%s")' % url, timeout=30)
    assert "CIRCLE-TLS-OK" not in out, out

    # Trusting the server's CA makes the verified request succeed, which also
    # proves the IP-SAN hostname check accepts 10.0.2.2.
    _install_ca(con, cert)
    assert con.send_line('WEB TLS CA "A:/CA.PEM"') == ""
    out = con.send_line('PRINT WEB$(HTTP GET "%s")' % url, timeout=30)
    assert "CIRCLE-TLS-OK" in out, out

    # WEB TLS NOVERIFY is the explicit insecure opt-out.
    assert con.send_line("WEB TLS NOVERIFY") == ""
    out = con.send_line('PRINT WEB$(HTTP GET "%s")' % url, timeout=30)
    assert "CIRCLE-TLS-OK" in out, out
