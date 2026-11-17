"""HUB engine (#1525): index, states, install/update/remove and sha256.

These drive the native stdio build against a hermetic loopback HTTP server.
Native stdio can reach 127.0.0.1 directly, so this is not the QEMU SLIRP case
and must not be gated on MMCORE_LIVE_NET.
"""

import hashlib
import http.server
import io
import json
import os
import shutil
import socket
import subprocess
import threading
import time
import zipfile

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("MMB_NATIVE_BIN") or os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)

TINY_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452"
    "000000010000000108060000001f15c4"
    "890000000d49444154789c6360000000"
    "020001e221bc330000000049454e44ae"
    "426082"
)


def make_app(name, title, version, main="PRINT \"APP RAN\"\n", minver="",
             pad=0):
    """Deterministic stored zip with MAIN.BAS first and PACKAGE.INF last."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("MAIN.BAS", main)
        if pad:
            zf.writestr("DATA.BIN", b"\0" * pad)
        zf.writestr(
            "PACKAGE.INF",
            "name=%s\nmain=MAIN.BAS\ntitle=%s\nversion=%s\nminver=%s\n"
            % (name, title, version, minver),
        )
    return buf.getvalue()


APPS = {
    "jetpack": make_app("jetpack", "Jetpack", "1.2", 'PRINT "JETPACK RAN"\n'),
    "puzzle": make_app("puzzle", "Puzzle", "2.0", 'PRINT "PUZZLE RAN"\n'),
    "future": make_app("future", "Future", "1.0", 'PRINT "FUTURE RAN"\n',
                       minver="0.400.0"),
    "badsum": make_app("badsum", "Bad Sum", "0.1", 'PRINT "BADSUM RAN"\n'),
    # Larger than the 1600-byte transport read (#1532) so a download always
    # crosses several progress callbacks, never just one coalesced read.
    "bulk": make_app("bulk", "Bulk", "1.0", 'PRINT "BULK RAN"\n', pad=8192),
}


def app_entry(app_id, category="OTHER", minver="", sha=None, size=None):
    data = APPS[app_id]
    return {
        "id": app_id,
        "file": "apps/%s.app" % app_id,
        "title": app_id.title(),
        "author": "tests",
        "category": category,
        "version": "1.2" if app_id == "jetpack" else (
            "2.0" if app_id == "puzzle" else "1.0"),
        "summary": "fixture %s" % app_id,
        "minver": minver,
        "size": len(data) if size is None else size,
        "sha256": hashlib.sha256(data).hexdigest() if sha is None else sha,
        "updated": "2026-11-17T03:00:00Z",
        "icon": "info/%s/icon.png" % app_id,
        "thumb": "",
        "shots": [],
        "about": "",
    }


def index_doc(schema=1, apps=None):
    return {
        "schema": schema,
        "generated": "2026-11-17T03:00:00Z",
        "categories": [
            "GAMES", "DEMOS", "CREATIVE", "AUDIO", "PRODUCTIVITY",
            "DEVELOPMENT", "UTILITIES", "INTERNET", "LEARNING", "OTHER",
        ],
        "apps": apps if apps is not None else [
            app_entry("jetpack", category="GAMES"),
            app_entry("puzzle"),
            app_entry("future", minver="0.400.0"),
            app_entry("badsum", sha="0" * 64),
        ],
    }


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="application/octet-stream", delay=0.0):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if delay > 0 and len(body) > 256:
                for i in range(0, len(body), 512):
                    self.wfile.write(body[i:i + 512])
                    self.wfile.flush()
                    time.sleep(delay)
            else:
                self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        self.server.requests.append(self.path)
        route = self.server.routes.get(self.path)
        if route is None:
            self._send(404, b"missing")
            return
        code, body, ctype = route
        self._send(code, body, ctype, self.server.delays.get(self.path, 0.0))

    def log_message(self, *args):
        pass


class HubServer:
    def __init__(self, tmp_path):
        self.routes = {}
        self.delays = {}
        self.requests = []
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.srv.daemon_threads = True
        self.srv.routes = self.routes
        self.srv.delays = self.delays
        self.srv.requests = self.requests
        self.thread = threading.Thread(target=self.srv.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.add_index("/index.json", index_doc())

    @property
    def base(self):
        return "http://127.0.0.1:%d/" % self.srv.server_address[1]

    def add_index(self, path, doc):
        self.routes[path] = (200, json.dumps(doc).encode(), "application/json")

    def add_apps(self):
        for app_id, data in APPS.items():
            self.routes["/apps/%s.app" % app_id] = (
                200, data, "application/octet-stream")
            self.routes["/info/%s/icon.png" % app_id] = (
                200, TINY_PNG, "image/png")

    def throttle(self, path, delay):
        """Pause `delay` seconds between 512-byte body chunks."""
        self.delays[path] = delay

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=5)


@pytest.fixture(scope="module")
def mmb():
    if not os.environ.get("MMB_NATIVE_BIN"):
        build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


@pytest.fixture()
def hub(tmp_path):
    s = HubServer(tmp_path)
    s.add_apps()
    try:
        yield s
    finally:
        s.close()


def run(mmb_bin, lines, root, env=None, after=None, timeout=120):
    """Run numbered lines plus RUN, then optional interactive prompt lines."""
    program = "\n".join("%d %s" % (10 * (i + 1), line)
                        for i, line in enumerate(lines))
    program += "\nRUN\n"
    if after:
        program += "".join(line + "\n" for line in after)
    full_env = dict(os.environ, MMB_DRIVE_ROOT=str(root),
                    MMCORE_RELEASE_VERSION="0.312.0")
    if env:
        full_env.update(env)
    proc = subprocess.run(
        [mmb_bin],
        input=program,
        text=True,
        capture_output=True,
        timeout=timeout,
        env=full_env,
    )
    return proc.stdout + proc.stderr


def set_url(base):
    return 'OPTION HUB URL "%s"' % base


def preinstall(root, app_id, data):
    apps = os.path.join(str(root), "C", "APPS")
    os.makedirs(apps, exist_ok=True)
    path = os.path.join(apps, app_id + ".APP")
    with open(path, "wb") as fh:
        fh.write(data)
    return path


def test_load_index_and_states(mmb, hub, tmp_path):
    out = run(mmb, [set_url(hub.base), "HUB LIST"], tmp_path)
    assert "jetpack" in out and "NOT INSTALLED" in out, out
    assert "puzzle" in out, out
    assert "future" in out, out
    assert "NEEDS NEWER" in out, out


def test_install_writes_file_and_app_runs(mmb, hub, tmp_path):
    apps_dir = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps_dir, exist_ok=True)
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB INSTALL "jetpack"',
    ], tmp_path, after=["JETPACK"])
    assert "INSTALLED" in out, out
    assert "JETPACK RAN" in out, out
    path = os.path.join(apps_dir, "jetpack.APP")
    assert os.path.isfile(path)
    with open(path, "rb") as fh:
        assert fh.read() == APPS["jetpack"]


def test_checksum_mismatch_writes_nothing(mmb, hub, tmp_path):
    apps_dir = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps_dir, exist_ok=True)
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB INSTALL "badsum"',
    ], tmp_path)
    assert "CHECKSUM FAILED" in out, out
    assert not os.path.exists(os.path.join(apps_dir, "badsum.APP"))
    assert not os.path.exists(os.path.join(apps_dir, "badsum.APP.tmp"))


def test_update_state_and_update(mmb, hub, tmp_path):
    old = make_app("jetpack", "Jetpack", "1.0", 'PRINT "OLD"\n')
    preinstall(tmp_path, "jetpack", old)
    listed = run(mmb, ['OPTION PATH "C:/APPS/"', set_url(hub.base),
                       "HUB LIST"], tmp_path)
    assert "UPDATE" in listed, listed
    out = run(mmb, ['OPTION PATH "C:/APPS/"', set_url(hub.base),
                    "HUB UPDATE"], tmp_path)
    assert "INSTALLED" in out, out
    with open(os.path.join(str(tmp_path), "C", "APPS", "jetpack.APP"),
              "rb") as fh:
        assert fh.read() == APPS["jetpack"]


def test_installed_without_version_is_older(mmb, hub, tmp_path):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("MAIN.BAS", 'PRINT "OLD"\n')
        zf.writestr("PACKAGE.INF", "name=jetpack\nmain=MAIN.BAS\n")
    apps = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, "renamed.APP"), "wb") as fh:
        fh.write(buf.getvalue())
    out = run(mmb, ['OPTION PATH "C:/APPS/"', set_url(hub.base),
                    "HUB LIST"], tmp_path)
    assert "UPDATE" in out, out


def test_inf_name_matches_id(mmb, hub, tmp_path):
    apps = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, "totally-other-name.APP"), "wb") as fh:
        fh.write(make_app("jetpack", "Jetpack", "1.2"))
    out = run(mmb, ['OPTION PATH "C:/APPS/"', set_url(hub.base),
                    "HUB LIST"], tmp_path)
    assert "INSTALLED" in out, out


def test_needs_newer_install_refused(mmb, hub, tmp_path):
    apps_dir = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps_dir, exist_ok=True)
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB INSTALL "future"',
    ], tmp_path)
    assert "NEEDS MMCORE 0.400.0" in out, out
    assert not os.path.exists(os.path.join(apps_dir, "future.APP"))


def test_404_is_server_error(mmb, hub, tmp_path):
    hub.routes["/bad404/index.json"] = (404, b"no", "text/plain")
    out = run(mmb, [set_url(hub.base + "bad404/"), "HUB LIST"], tmp_path)
    assert "SERVER ERROR 404" in out, out


def test_offline_when_no_server(mmb, tmp_path):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    out = run(mmb, [set_url("http://127.0.0.1:%d/" % port), "HUB LIST"],
              tmp_path)
    assert "OFFLINE" in out, out


def test_schema_two_is_unsupported(mmb, hub, tmp_path):
    hub.add_index("/v2/index.json", index_doc(schema=2))
    out = run(mmb, [set_url(hub.base + "v2/"), "HUB LIST"], tmp_path)
    assert "UNSUPPORTED INDEX" in out, out
    assert "newer mmcore" in out, out


def test_remove_deletes_file(mmb, hub, tmp_path):
    preinstall(tmp_path, "jetpack", make_app("jetpack", "Jetpack", "1.0"))
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB REMOVE "jetpack"',
        "HUB LIST",
    ], tmp_path)
    assert "removed" in out, out
    assert not os.path.exists(os.path.join(str(tmp_path), "C", "APPS",
                                           "jetpack.APP"))
    assert "NOT INSTALLED" in out, out


def test_cancel_leaves_no_temp(mmb, hub, tmp_path):
    apps_dir = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps_dir, exist_ok=True)
    # The body must exceed one 1600-byte transport read (#1532): with a tiny
    # fixture the whole response can arrive in a single chunk, so make the
    # download cross a real chunk boundary before the cancel threshold hits.
    hub.add_index("/index.json", index_doc(apps=[app_entry("bulk")]))
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB INSTALL "bulk"',
    ], tmp_path, env={"MMCORE_HUB_CANCEL_AFTER": "10"})
    assert "CANCELLED" in out, out
    assert not os.path.exists(os.path.join(apps_dir, "bulk.APP"))
    leftovers = [n for n in os.listdir(apps_dir) if n.endswith(".tmp")]
    assert leftovers == [], leftovers


def test_cancel_tiny_body_still_offers_progress(mmb, hub, tmp_path):
    """A body that arrives whole with the headers must still reach the cancel
    callback once before the install commits (#1532)."""
    apps_dir = os.path.join(str(tmp_path), "C", "APPS")
    os.makedirs(apps_dir, exist_ok=True)
    out = run(mmb, [
        'OPTION PATH "C:/APPS/"',
        set_url(hub.base),
        'HUB INSTALL "jetpack"',
    ], tmp_path, env={"MMCORE_HUB_CANCEL_AFTER": "10"})
    assert "CANCELLED" in out, out
    assert not os.path.exists(os.path.join(apps_dir, "jetpack.APP"))
    leftovers = [n for n in os.listdir(apps_dir) if n.endswith(".tmp")]
    assert leftovers == [], leftovers


def test_empty_relpath_makes_no_request(mmb, hub, tmp_path):
    out = run(mmb, [
        set_url(hub.base),
        "HUB LIST",
        'HUB FETCH ""',
        'HUB FETCH "info/jetpack/icon.png"',
    ], tmp_path)
    assert "FETCH 0" in out, out
    assert "FETCH %d" % len(TINY_PNG) in out, out
    non_index = [p for p in hub.requests if not p.endswith("index.json")]
    assert non_index == ["/info/jetpack/icon.png"], hub.requests


def test_option_hub_url_trailing_slash_and_default(mmb, tmp_path):
    out = run(mmb, [
        'OPTION HUB URL "http://example.test"',
        "OPTION LIST",
    ], tmp_path)
    assert 'OPTION HUB URL "http://example.test/"' in out, out
    out = run(mmb, [
        'OPTION HUB URL "http://example.test"',
        "OPTION HUB URL DEFAULT",
        "OPTION LIST ALL",
    ], tmp_path)
    assert 'OPTION HUB URL "https://hub.mmcore.dev/"' in out, out
    # Saved in .mmbasic.ini like the other string options.
    run(mmb, ['OPTION HUB URL "http://persisted.test"'], tmp_path)
    out = run(mmb, ["OPTION LIST"], tmp_path)
    assert 'OPTION HUB URL "http://persisted.test/"' in out, out
