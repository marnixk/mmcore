"""#1128 phase 1: OPTION NETWORK DRIVE (SMB share mounted at Z:).

Two layers are covered here:

* Parsing / persistence (always run): ``OPTION NETWORK DRIVE`` normalises the
  UNC, stores the NTLM hash in ``.mmbasic.ini``, never prints the password,
  survives ``OPTION RESET`` and clears on ``OFF``. These drive the native
  interpreter and need no server.
* Live SMB behaviour (opt-in): a local Samba ``smbd`` is started on a
  non-privileged port with a guest and a user share, and DIR / SAVE / LOAD /
  OPEN / COPY / KILL / MKDIR / NAME are exercised on Z:. These need a real
  Samba ``smbd`` and ``MMCORE_LIVE_NET=1`` (the same gate the other live
  network tests use); they skip otherwise.

A host-level C test (``tests/netdrive_host.c``) checks the UNC/hash helpers
directly, so the parsing logic is covered even where no Samba is installed.
"""
from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

import pytest

from native_harness import build_native
from net_util import live_net_enabled

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")
SMB_LIB = os.path.join(REPO, "mmbasic", "third_party", "libsmb2")

INCLUDES = [
    "-DMMB_PLATFORM_POSIX",
    "-DMMB_HAVE_SMB2=1",
    "-DHAVE_CONFIG_H",
    "-I",
    os.path.join(REPO, "native"),
    "-I",
    os.path.join(REPO, "mmbasic", "include"),
    "-I",
    os.path.join(REPO, "mmbasic", "src"),
    "-I",
    os.path.join(REPO, "mmbasic", "third_party"),
    "-I",
    os.path.join(REPO, "console"),
    "-I",
    SMB_LIB,
    "-I",
    os.path.join(SMB_LIB, "lib"),
]

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def _clean(s: str) -> str:
    return _ANSI.sub("", s)


@pytest.fixture(scope="module")
def mmb_native() -> str:
    if shutil.which("cc") is None or shutil.which("make") is None:
        pytest.skip("needs a host C toolchain (cc/make)")
    build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


def run_mmb(binary: str, script: str, root: str, timeout: int = 90,
            extra_env: dict | None = None) -> str:
    """Run a native interpreter session with a fresh drive root."""
    env = dict(os.environ, MMB_DRIVE_ROOT=root)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.run(
        [binary],
        input=script,
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )
    return _clean(proc.stdout + proc.stderr)


def _ini(root: str) -> str:
    path = os.path.join(root, "C", ".mmbasic.ini")
    if not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


# --------------------------------------------------------------------------
# Parsing / persistence (no server needed)
# --------------------------------------------------------------------------

def test_option_list_never_prints_password(mmb_native, tmp_path):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\host\\share\\sub","user","s3cr3t"\n'
        "OPTION LIST\n",
        root,
    )
    assert 'OPTION NETWORK DRIVE "\\\\host\\share\\sub","user"' in out
    assert "s3cr3t" not in out
    assert "ntlm" not in out


def test_ini_stores_ntlm_hash_not_password(mmb_native, tmp_path):
    root = str(tmp_path / "root")
    run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\host\\share","user","secret"\n',
        root,
    )
    ini = _ini(root)
    assert "pass=ntlm:878d8014606cda29677a44efa1353fc7" in ini
    assert "secret" not in ini


@pytest.mark.parametrize(
    "typed,shown",
    [
        ("\\\\\\\\host\\\\share", "\\\\host\\share"),  # doubled backslashes
        ("//host/share/sub", "\\\\host\\share\\sub"),  # forward slashes
        ("smb://host:4455/share", "\\\\host:4455\\share"),  # scheme + port
    ],
)
def test_unc_normalisation(mmb_native, tmp_path, typed, shown):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "%s"\nOPTION LIST\n' % typed,
        root,
    )
    assert 'OPTION NETWORK DRIVE "%s"' % shown in out


def test_option_reset_preserves_credentials(mmb_native, tmp_path):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\host\\share","user","pw"\n'
        "OPTION RESET\n"
        "OPTION LIST\n",
        root,
    )
    assert 'OPTION NETWORK DRIVE "\\\\host\\share","user"' in out
    assert "pw" not in out.split("OPTION NETWORK DRIVE")[-1]


def test_option_off_clears(mmb_native, tmp_path):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\host\\share"\n'
        "OPTION NETWORK DRIVE OFF\n"
        "OPTION LIST\n",
        root,
    )
    assert "NETWORK DRIVE" not in out


def test_drive_line_does_not_connect(mmb_native, tmp_path):
    """DRIVE shows Z: as configured but not connected, without connecting."""
    root = str(tmp_path / "root")
    # 127.0.0.1:1 refuses immediately; if DRIVE connected it would raise.
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\127.0.0.1:1\\share"\n'
        "DRIVE\n"
        'PRINT "still here"\n',
        root,
        timeout=30,
    )
    assert "Z: SMB \\\\127.0.0.1:1\\share (not connected)" in out
    assert "still here" in out
    assert "NETWORK DRIVE:" not in out


def test_connect_failure_does_not_leak_password(mmb_native, tmp_path):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\127.0.0.1:1\\share","user","topsecret"\n'
        'DIR "Z:/"\n',
        root,
        timeout=30,
    )
    assert "?NETWORK DRIVE: host unreachable" in out
    assert "topsecret" not in out


# --------------------------------------------------------------------------
# Host C test for the pure UNC / hash helpers
# --------------------------------------------------------------------------

def test_unc_and_hash_host(tmp_path):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    exe = str(tmp_path / "netdrive_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            *INCLUDES,
            "-o",
            exe,
            os.path.join(REPO, "tests", "netdrive_host.c"),
            os.path.join(REPO, "mmbasic", "src", "netdrive_unc.c"),
            os.path.join(SMB_LIB, "lib", "md4c.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "FAIL" not in out.stdout, out.stdout
    assert out.stdout.count("ok ") >= 12


# --------------------------------------------------------------------------
# Live Samba server + behaviour
# --------------------------------------------------------------------------

def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _real_samba_smbd() -> str | None:
    """A Samba ``smbd`` (not Apple's macOS smbd) with its companion tools."""
    smbd = shutil.which("smbd")
    if not smbd:
        return None
    if shutil.which("smbpasswd") is None:
        return None
    return smbd


class SambaServer:
    def __init__(self, tmpdir: str):
        self.dir = tmpdir
        self.port = _free_port()
        self.proc: subprocess.Popen | None = None
        self.guest = os.path.join(tmpdir, "guest")
        self.user = os.path.join(tmpdir, "user")
        self.priv = os.path.join(tmpdir, "private")
        self.user_name = "mmtest"
        self.user_pass = "mmtestpass"
        for d in (self.guest, self.user, self.priv):
            os.makedirs(d, exist_ok=True)
            os.chmod(d, 0o777)
        with open(os.path.join(self.guest, "hello.txt"), "w") as fh:
            fh.write("hello from guest\n")

    def _config(self) -> str:
        for sub in ("lock", "state", "private", "cache"):
            os.makedirs(os.path.join(self.priv, sub), exist_ok=True)
        return f"""
[global]
   workgroup = MMTEST
   server string = mmcore test
   security = user
   map to guest = Bad User
   smb ports = {self.port}
   interfaces = 127.0.0.1
   bind interfaces only = yes
   server min protocol = SMB2
   pid directory = {self.priv}
   lock directory = {self.priv}/lock
   state directory = {self.priv}/state
   private dir = {self.priv}/private
   cache directory = {self.priv}/cache
   log file = {self.priv}/log.smbd
   max log size = 100
   disable spoolss = yes
   load printers = no
   printing = bsd
   printcap name = /dev/null

[guest]
   path = {self.guest}
   browseable = yes
   read only = no
   guest ok = yes
   force user = nobody
   create mask = 0666
   directory mask = 0777

[user]
   path = {self.user}
   browseable = yes
   read only = no
   valid users = {self.user_name}
   create mask = 0666
   directory mask = 0777
"""

    def _add_user(self) -> bool:
        # Try as the current user, then via passwordless sudo.
        runs = [
            [shutil.which("smbpasswd") or "smbpasswd", "-a", "-s",
             self.user_name],
        ]
        if shutil.which("sudo"):
            runs.append(["sudo", "-n", "smbpasswd", "-a", "-s",
                         self.user_name])
        for cmd in runs:
            try:
                p = subprocess.run(
                    cmd,
                    input="%s\n%s\n" % (self.user_pass, self.user_pass),
                    text=True,
                    capture_output=True,
                    timeout=30,
                )
            except (OSError, subprocess.SubprocessError):
                continue
            if p.returncode == 0:
                return True
        return False

    def start(self) -> "SambaServer":
        conf = os.path.join(self.dir, "smb.conf")
        with open(conf, "w") as fh:
            fh.write(self._config())
        self.user_ok = self._add_user()
        smbd = _real_samba_smbd()
        self.proc = subprocess.Popen(
            [smbd, "-F", "--no-process-group", "-s", conf],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("smbd exited early")
            try:
                s = socket.create_connection(("127.0.0.1", self.port), 0.5)
                s.close()
                return self
            except OSError:
                time.sleep(0.2)
        raise RuntimeError("smbd did not start listening")

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


@pytest.fixture(scope="module")
def samba(tmp_path_factory):
    if not live_net_enabled():
        pytest.skip("set MMCORE_LIVE_NET=1 to run live SMB tests")
    if _real_samba_smbd() is None:
        pytest.skip("a real Samba smbd/smbpasswd is not available")
    d = str(tmp_path_factory.mktemp("samba"))
    try:
        srv = SambaServer(d).start()
    except Exception as exc:  # setup environment varies wildly
        pytest.skip("could not start smbd: %s" % exc)
    try:
        yield srv
    finally:
        srv.stop()


def _guest_unc(srv: SambaServer) -> str:
    return "\\\\127.0.0.1:%d\\guest" % srv.port


def _user_unc(srv: SambaServer) -> str:
    return "\\\\127.0.0.1:%d\\user" % srv.port


def test_z_guest_round_trip(samba, mmb_native, tmp_path):
    """DIR / OPEN / COPY / MKDIR / NAME / KILL / LOAD / SAVE on Z: (guest)."""
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'DIR "Z:/"\n'
        + 'OPEN "Z:/out.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "hello world"\n'
        + "CLOSE #1\n"
        + 'COPY "Z:/out.txt" "Z:/copy.txt"\n'
        + 'MKDIR "Z:/dir1"\n'
        + 'NAME "Z:/copy.txt" AS "Z:/renamed.txt"\n'
        + 'KILL "Z:/out.txt"\n'
        + 'DIR "Z:/"\n'
        # This fork's LOAD is the image command; RUN "file" loads+runs a
        # program, which exercises the Z: read path (size + read_at).
        + '10 PRINT "HELLO"\n'
        + 'SAVE "Z:/P.BAS"\n'
        + "NEW\n"
        + 'RUN "Z:/P.BAS"\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=180)
    assert "?FILE" not in out
    assert "?DIRECTORY" not in out
    assert "hello.txt" in out
    assert "renamed.txt" in out
    assert "dir1/" in out
    assert "HELLO" in out


def test_z_user_share_authenticated(samba, mmb_native, tmp_path):
    if not getattr(samba, "user_ok", False):
        pytest.skip("could not create the Samba test user")
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s","%s","%s"\n'
        % (_user_unc(samba), samba.user_name, samba.user_pass)
        + 'OPEN "Z:/u.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "authed"\n'
        + "CLOSE #1\n"
        + 'DIR "Z:/"\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=120)
    assert "u.txt" in out
    assert "?NETWORK DRIVE" not in out


def test_connect_failure_is_short_and_clear(samba, mmb_native, tmp_path):
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\127.0.0.1:1\\nope","u","pw"\nDIR "Z:/"\n',
        root,
        timeout=30,
    )
    assert "?NETWORK DRIVE: host unreachable" in out
    assert "pw" not in out


def test_smb_does_not_disturb_term_connection(samba, mmb_native, tmp_path):
    """The SMB context owns its own socket: an open CONNECT/TERM session keeps
    working across a Z: operation and an SMB disconnect (#1128)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def echo():
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        f = conn.makefile("rwb")
        while True:
            line = f.readline()
            if not line:
                break
            f.write(b"ECHO:" + line)
            f.flush()
        conn.close()

    th = threading.Thread(target=echo, daemon=True)
    th.start()

    root = str(tmp_path / "root")
    script = (
        'OPEN "127.0.0.1:%d" AS #1\n' % port
        + 'PRINT #1, "one"\n'
        + "INPUT #1, a$\n"
        + 'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'DIR "Z:/"\n'
        + 'PRINT #1, "two"\n'
        + "INPUT #1, b$\n"
        + "OPTION NETWORK DRIVE OFF\n"
        + 'PRINT #1, "three"\n'
        + "INPUT #1, c$\n"
        + 'PRINT "T1:"; a$; " T2:"; b$; " T3:"; c$\n'
        + "CLOSE #1\n"
    )
    out = run_mmb(mmb_native, script, root, timeout=120)
    srv.close()
    assert "T1:one T2:two T3:three" in out
