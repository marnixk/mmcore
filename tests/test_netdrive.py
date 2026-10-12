"""#1128 network drive (SMB share mounted at Z:), phases 1 and 3 (#1133).

Two layers are covered here:

* Parsing / persistence (always run): ``OPTION NETWORK DRIVE`` normalises the
  UNC, stores the NTLM hash in ``.mmbasic.ini``, never prints the password,
  survives ``OPTION RESET`` and clears on ``OFF``. These drive the native
  interpreter and need no server.
* Live SMB behaviour (opt-in): a local Samba ``smbd`` is started on a
  non-privileged port with a guest and a user share, and DIR / SAVE / LOAD /
  OPEN / COPY / KILL / MKDIR / NAME are exercised on Z:, plus the #1133
  reconnect-after-drop path and the cached PRINT#/INPUT# handles. These need a
  real Samba ``smbd`` and ``MMCORE_LIVE_NET=1`` (the same gate the other live
  network tests use); they skip otherwise.

A host-level C test (``tests/netdrive_host.c``) checks the UNC/hash helpers
directly, so the parsing logic is covered even where no Samba is installed.
"""
from __future__ import annotations

import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

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


def test_repeated_connect_failure_stays_short(mmb_native, tmp_path):
    """Two failed Z: uses in one session both stay short and never crash the
    teardown/reconnect path (#1133)."""
    root = str(tmp_path / "root")
    out = run_mmb(
        mmb_native,
        'OPTION NETWORK DRIVE "\\\\127.0.0.1:1\\share","user","topsecret"\n'
        'DIR "Z:/"\n'
        'PRINT "between"\n'
        'DIR "Z:/"\n'
        "PRINT \"after\"\n",
        root,
        timeout=40,
    )
    assert out.count("?NETWORK DRIVE: host unreachable") >= 2
    assert "topsecret" not in out
    assert "between" in out
    assert "after" in out


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
    """A Samba ``smbd`` (not Apple's macOS smbd) with its companion tools.

    ``smbpasswd`` only comes with real Samba (never Apple's), so it gates the
    check; Homebrew renames the server to ``samba-dot-org-smbd`` to avoid
    shadowing ``/usr/sbin/smbd``, while Linux ships the plain ``smbd`` name.
    """
    if shutil.which("smbpasswd") is None:
        return None
    return shutil.which("samba-dot-org-smbd") or shutil.which("smbd")


class SambaServer:
    def __init__(self, tmpdir: str):
        self.dir = tmpdir
        self.port = _free_port()
        self.proc: subprocess.Popen | None = None
        self.guest = os.path.join(tmpdir, "guest")
        self.user = os.path.join(tmpdir, "user")
        # A share with Samba's default (owner-only) create mask, so an anonymous
        # guest session can create files but cannot reopen them for writing --
        # the case that needs the recreate fallback in netfs_smb2.c.
        self.strict = os.path.join(tmpdir, "strict")
        # Samba's private dir holds UNIX sockets whose path must stay under the
        # ~104-char sun_path limit; pytest's tmp tree is far too deep, so keep
        # the server state in a short /tmp dir (#1133).
        self.priv = tempfile.mkdtemp(prefix="mmbsmb-", dir="/tmp")
        self.user_name = "mmtest"
        self.user_pass = "mmtestpass"
        for d in (self.guest, self.user, self.strict, self.priv):
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

[strict]
   path = {self.strict}
   browseable = yes
   read only = no
   guest ok = yes
   create mask = 0644
   directory mask = 0755
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

    def _terminate(self) -> None:
        # Homebrew smbd can wedge in Popen.wait(); SIGKILL plus a poll loop is
        # enough for a throwaway test server and never blocks the suite.
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.kill()
            except OSError:
                pass
            deadline = time.time() + 5
            while time.time() < deadline and self.proc.poll() is None:
                time.sleep(0.05)
        self.proc = None

    def stop(self) -> None:
        self._terminate()
        shutil.rmtree(self.priv, ignore_errors=True)

    def restart(self) -> "SambaServer":
        """Bounce smbd on the same port (reconnect tests, #1133)."""
        self._terminate()
        return self.start()


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


def _strict_unc(srv: SambaServer) -> str:
    return "\\\\127.0.0.1:%d\\strict" % srv.port


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


def test_z_repeated_mkdir_one_session(samba, mmb_native, tmp_path):
    """Every MKDIR in a session succeeds, not just the first, and one after a
    non-mkdir Z: op works too (#1139).

    Samba's mkdir_internal() can return a spurious NT_STATUS_ACCESS_DENIED
    *after* creating the directory (it builds it under a temporary name and
    RENAMEAT()s it), which used to make a later MKDIR report ?DIRECTORY while
    still leaving the folder on the share.
    """
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'MKDIR "Z:/m1"\n'
        + 'MKDIR "Z:/m2"\n'
        + 'OPEN "Z:/x.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "x"\n'
        + "CLOSE #1\n"
        + 'MKDIR "Z:/m3"\n'
        + 'DIR "Z:/"\n'
        + 'PRINT "AFTER"\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=120)
    assert "?DIRECTORY" not in out
    assert "?FILE" not in out
    assert "AFTER" in out
    for name in ("m1", "m2", "m3"):
        assert "%s/" % name in out
        assert os.path.isdir(os.path.join(samba.guest, name))


def test_z_mkdir_existing_still_reports_directory(samba, mmb_native, tmp_path):
    """A MKDIR on a directory that truly pre-exists is still ?DIRECTORY, so the
    #1139 fix does not swallow the duplicate case."""
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'MKDIR "Z:/dup"\n'
        + 'MKDIR "Z:/dup"\n'
        + 'PRINT "DONE"\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=120)
    assert out.count("?DIRECTORY") == 1
    assert "DONE" in out
    assert os.path.isdir(os.path.join(samba.guest, "dup"))


def test_z_write_existing_on_owner_only_share(samba, mmb_native, tmp_path):
    """Overwrite and append an existing file on a share whose files are
    owner-only (Samba's default create mask).

    An anonymous/guest session on such a share maps to the read-only Everyone
    SID: it can create and delete but not reopen an existing file for writing
    (only a *named* guest logon owns the file). netfs_smb2.c must fall back to
    recreating the file, copying the old bytes for an append, instead of
    reporting ?FILE -- which is what made EDIT save fail on a real Samba box.
    """
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _strict_unc(samba)
        + 'OPEN "Z:/f.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "AAA"\n'
        + "CLOSE #1\n"
        # Overwrite the file it just created (reopen denied -> recreate).
        + 'OPEN "Z:/f.txt" FOR OUTPUT AS #2\n'
        + 'PRINT #2, "BBB"\n'
        + "CLOSE #2\n"
        + 'OPEN "Z:/f.txt" FOR INPUT AS #3\n'
        + "LINE INPUT #3, a$\n"
        + "CLOSE #3\n"
        + 'PRINT "AFTER_OVERWRITE="; a$\n'
        # Append to the existing file (reopen denied -> recreate preserving).
        + 'OPEN "Z:/f.txt" FOR APPEND AS #4\n'
        + 'PRINT #4, "CCC"\n'
        + "CLOSE #4\n"
        + 'OPEN "Z:/f.txt" FOR INPUT AS #5\n'
        + "LINE INPUT #5, b$\n"
        + "LINE INPUT #5, c$\n"
        + "CLOSE #5\n"
        + 'PRINT "L1="; b$\n'
        + 'PRINT "L2="; c$\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=180)
    assert "?FILE" not in out
    assert "AFTER_OVERWRITE=BBB" in out
    assert "L1=BBB" in out
    assert "L2=CCC" in out


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


# --------------------------------------------------------------------------
# Reconnect + open-handle caching (#1133)
# --------------------------------------------------------------------------

def _bounce_when_marker(samba: SambaServer, marker: str,
                        timeout: float = 30.0) -> threading.Thread:
    """Restart the share once the guest drops a handshake file.

    The native interpreter runs the whole script as one subprocess, so the only
    way to drop the share *mid-session* is for a watcher thread to bounce smbd
    while the guest busy-waits in ``PAUSE``.
    """
    def worker() -> None:
        deadline = time.time() + timeout
        while time.time() < deadline and not os.path.exists(marker):
            time.sleep(0.1)
        if not os.path.exists(marker):
            return
        try:
            samba.restart()
        except Exception:  # the assertion below reports the failure
            pass

    th = threading.Thread(target=worker, daemon=True)
    th.start()
    return th


def test_z_reconnects_after_share_restart(samba, mmb_native, tmp_path):
    """An idempotent Z: op after the share drops reconnects once and retries,
    keeping the pre-drop file and writing the post-drop one."""
    root = str(tmp_path / "root")
    os.makedirs(os.path.join(root, "C"), exist_ok=True)
    marker = os.path.join(root, "C", "phase1.done")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'OPEN "Z:/before.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "before"\n'
        + "CLOSE #1\n"
        + 'DIR "Z:/"\n'
        + 'OPEN "C:/phase1.done" FOR OUTPUT AS #2\n'
        + 'PRINT #2, "go"\n'
        + "CLOSE #2\n"
        + "PAUSE 15000\n"
        + 'PRINT "dropped"\n'
        + 'DIR "Z:/"\n'
        + 'OPEN "Z:/after.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "after"\n'
        + "CLOSE #1\n"
        + 'DIR "Z:/"\n'
    )
    th = _bounce_when_marker(samba, marker)
    out = run_mmb(mmb_native, script, root, timeout=120)
    th.join(timeout=10)
    assert "?NETWORK DRIVE" not in out
    assert "before.txt" in out
    assert "after.txt" in out
    with open(os.path.join(samba.guest, "after.txt")) as fh:
        assert fh.read().strip() == "after"


def test_z_dropped_write_fails_file(samba, mmb_native, tmp_path):
    """A write that finds the share already dropped fails with ?FILE and is not
    replayed (#1133)."""
    root = str(tmp_path / "root")
    os.makedirs(os.path.join(root, "C"), exist_ok=True)
    marker = os.path.join(root, "C", "phase1.done")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'OPEN "Z:/before.txt" FOR OUTPUT AS #1\n'
        + 'PRINT #1, "before"\n'
        + "CLOSE #1\n"
        + 'DIR "Z:/"\n'
        + 'OPEN "C:/phase1.done" FOR OUTPUT AS #2\n'
        + 'PRINT #2, "go"\n'
        + "CLOSE #2\n"
        + "PAUSE 15000\n"
        + 'OPEN "Z:/late.txt" FOR OUTPUT AS #1\n'
    )
    th = _bounce_when_marker(samba, marker)
    out = run_mmb(mmb_native, script, root, timeout=120)
    th.join(timeout=10)
    assert "?FILE" in out
    assert not os.path.exists(os.path.join(samba.guest, "late.txt"))


def test_z_many_print_and_input_lines(samba, mmb_native, tmp_path):
    """The cached append handle keeps PRINT# offsets correct across many calls
    and INPUT# reads them back byte-for-byte (#1133)."""
    root = str(tmp_path / "root")
    lines = 40
    script = 'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
    script += 'OPEN "Z:/many.txt" FOR OUTPUT AS #1\n'
    for i in range(1, lines + 1):
        script += 'PRINT #1, "line %d"\n' % i
    script += "CLOSE #1\n"
    script += 'OPEN "Z:/many.txt" FOR INPUT AS #1\n'
    for i in range(1, lines + 1):
        script += "INPUT #1, x$\n"
        script += 'IF x$ <> "line %d" THEN PRINT "BAD %d:"; x$\n' % (i, i)
    script += "CLOSE #1\n"
    script += 'PRINT "DONE"\n'
    out = run_mmb(mmb_native, script, root, timeout=120)
    assert "BAD" not in out
    assert "?FILE" not in out
    assert "DONE" in out
    with open(os.path.join(samba.guest, "many.txt")) as fh:
        assert fh.read() == "".join("line %d\n" % i for i in range(1, lines + 1))


# --------------------------------------------------------------------------
# Large single transfer (#1142/#1143): negotiated chunking + pipelined window
# --------------------------------------------------------------------------

def _padded_png(pad: int = 1200 * 1024) -> bytes:
    """A small valid RGB PNG preceded by an ancillary tEXt chunk of `pad`
    bytes.

    The padding makes the file larger than the 1 MB per-PDU cap, so reading it
    through Z: in one call must slice it into several pipelined SMB2 reads. The
    padding sits *before* the IDAT, so a truncated read loses the image data
    and the decode fails -- the assertion actually checks the bytes arrived.
    uPNG skips unknown ancillary chunks, so the padding is transparent.
    """
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    w, h = 320, 240
    raw = bytearray()
    for _ in range(h):
        raw.append(0)  # filter type 0
        raw.extend(os.urandom(w * 3))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"tEXt", b"pad\x00" + os.urandom(pad))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


def test_z_large_read_slices_into_pdus(samba, mmb_native, tmp_path):
    """A Z: file bigger than one negotiated chunk is read correctly in one call.

    ``LOAD IMAGE`` reads the whole file with a single ``read_at`` (#1142 sizes
    that read from the negotiated max, capped at 1 MB; #1143 keeps several PDUs
    outstanding). The PNG's decompressed image is checked by uPNG's CRC, so a
    short or misordered read fails the load.
    """
    png = _padded_png()
    assert len(png) > (1 << 20), len(png)
    with open(os.path.join(samba.guest, "big.png"), "wb") as fh:
        fh.write(png)
    root = str(tmp_path / "root")
    script = (
        'OPTION NETWORK DRIVE "%s"\n' % _guest_unc(samba)
        + 'LOAD IMAGE "Z:/big.png"\n'
        + 'PRINT "LOADED"; PIXEL(0, 0)\n'
    )
    out = run_mmb(mmb_native, script, root, timeout=120)
    assert "?PNG" not in out
    assert "?FILE" not in out
    assert "?NETWORK DRIVE" not in out
    assert "LOADED" in out

