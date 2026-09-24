"""Reusable driver for the native SDL test harness (#633, PAINT wave 0C).

The native build (``native/mmcore``) can run a scripted session under the SDL
"dummy" video driver. ``MMB_SDL_HARNESS`` names a text file whose commands are
executed one per main-loop frame, so a poll-based app such as PAINT sees each
synthetic mouse move / button / key on its own frame. ``shot`` writes the
current software framebuffer to a PPM, which this module reads into pixels.

Typical use::

    s = NativeSession(tmp_path)
    s.feed("PAINT")                 # enter the app
    before = s.shot("before.ppm")
    s.move(100, 100)                # synthetic mouse move
    after = s.shot("after.ppm")
    s.quit()
    out = s.run()
    assert Ppm(before).pixel(100, 100) == (0, 0, 0)
    assert lum(Ppm(after).pixel(100, 100)) > 600

Only the SDL build is supported; tests should skip when ``native/mmcore`` was
not produced (no SDL2). Nothing here touches the QEMU suite.
"""
from __future__ import annotations

import fcntl
import hashlib
import os
import subprocess
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDL_BIN = os.path.join(REPO, "native", "mmcore")

# Serialise the native build across parallel pytest workers (and the
# ``test_linux_native`` fixture). Kept in the temp dir so it never litters the
# worktree; the name is keyed by the repo root so separate worktrees build
# independently.
_BUILD_LOCK = os.path.join(
    tempfile.gettempdir(),
    "mmcore-native-build-%s.lock"
    % hashlib.sha1(REPO.encode()).hexdigest()[:12],
)


def build_native() -> None:
    """Build ``native/mmbasic`` + ``native/mmcore`` (idempotent, serialised)."""
    with open(_BUILD_LOCK, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            subprocess.run(
                ["bash", os.path.join(REPO, "scripts", "build-native.sh")],
                cwd=REPO,
                check=True,
                capture_output=True,
            )
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def sdl_backend_available() -> bool:
    return os.path.isfile(SDL_BIN)


class Ppm:
    """A binary P6 PPM framebuffer capture with pixel accessors."""

    def __init__(self, path: str):
        with open(path, "rb") as fh:
            magic = fh.readline().strip()
            if magic != b"P6":
                raise ValueError("not a P6 PPM: %r" % magic)
            self.width, self.height = map(int, fh.readline().split())
            self.maxval = int(fh.readline())
            self.data = fh.read()
        want = self.width * self.height * 3
        if len(self.data) != want:
            raise ValueError("truncated PPM: %d != %d" % (len(self.data), want))

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """RGB triple at framebuffer pixel (x, y)."""
        i = (y * self.width + x) * 3
        return self.data[i], self.data[i + 1], self.data[i + 2]

    def nonblack(self) -> int:
        d = self.data
        return sum(
            1 for i in range(0, len(d), 3) if d[i] or d[i + 1] or d[i + 2]
        )


def lum(rgb: tuple[int, int, int]) -> int:
    return sum(rgb)


def is_black(rgb: tuple[int, int, int], tol: int = 40) -> bool:
    return all(c <= tol for c in rgb)


def is_white(rgb: tuple[int, int, int], lo: int = 200) -> bool:
    return all(c >= lo for c in rgb)


class NativeSession:
    """Launch ``native/mmcore`` with a scripted synthetic input session.

    Command builders (``feed``, ``move``, ``click``, ``key``, ``text``,
    ``shot``, ...) append to the session script. Calling them before
    :meth:`start` buffers the whole script; calling them after appends to the
    script file live, so a test can interleave injection and captures.
    """

    def __init__(self, workdir, env: dict | None = None, binary: str | None = None):
        self.workdir = str(workdir)
        os.makedirs(self.workdir, exist_ok=True)
        self.script_path = os.path.join(self.workdir, "harness.txt")
        self.binary = binary or SDL_BIN
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        if env:
            self.env.update(env)
        self._proc: subprocess.Popen | None = None
        self._fh = None
        self._buf: list[str] = []

    # ---- command builders -------------------------------------------------

    def feed(self, text: str) -> "NativeSession":
        """Type ``text`` plus Enter into the REPL / running program."""
        return self._emit("feed " + text)

    def move(self, x: int, y: int) -> "NativeSession":
        """Synthetic mouse move to framebuffer pixel (x, y)."""
        return self._emit("mouse %d %d" % (x, y))

    def down(self, button: str = "l") -> "NativeSession":
        return self._emit("down " + button)

    def up(self, button: str = "l") -> "NativeSession":
        return self._emit("up " + button)

    def click(self, button: str = "l") -> "NativeSession":
        """Press then release across two frames."""
        return self._emit("click " + button)

    def key(self, spec: str) -> "NativeSession":
        """Press a key; ``spec`` may carry modifiers, e.g. ``alt+x``."""
        return self._emit("key " + spec)

    def text(self, s: str) -> "NativeSession":
        return self._emit("text " + s)

    def shot(self, name: str) -> str:
        """Queue a framebuffer capture; returns the PPM path."""
        path = os.path.join(self.workdir, name)
        self._emit("shot " + path)
        return path

    def mark(self, name: str) -> str:
        path = os.path.join(self.workdir, name)
        self._emit("mark " + path)
        return path

    def quit(self) -> "NativeSession":
        return self._emit("quit")

    # ---- lifecycle --------------------------------------------------------

    def _emit(self, line: str) -> "NativeSession":
        self._buf.append(line)
        if self._fh:
            self._fh.write(line + "\n")
            self._fh.flush()
        return self

    def start(self) -> subprocess.Popen:
        if not os.path.isfile(self.binary):
            raise RuntimeError("native SDL build missing: %s" % self.binary)
        if os.path.exists(self.script_path):
            os.remove(self.script_path)
        self._fh = open(self.script_path, "w")
        for line in self._buf:
            self._fh.write(line + "\n")
        self._fh.flush()
        env = dict(self.env, MMB_SDL_HARNESS=self.script_path)
        self._proc = subprocess.Popen(
            [self.binary],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
        return self._proc

    def wait(self, timeout: float = 30.0) -> str:
        assert self._proc is not None, "session not started"
        out = self._proc.communicate(timeout=timeout)[0]
        if self._fh:
            self._fh.close()
            self._fh = None
        return out

    def run(self, timeout: float = 30.0) -> str:
        """Run the buffered script to completion and return stdout+stderr."""
        self.start()
        return self.wait(timeout)

    def stop(self, timeout: float = 10.0) -> None:
        if self._proc is None:
            return
        try:
            self._emit("quit")
            self.wait(timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()
        finally:
            if self._fh:
                self._fh.close()
                self._fh = None

    # ---- captures ---------------------------------------------------------

    def snapshot(self, name: str, timeout: float = 10.0) -> Ppm:
        """Capture the live framebuffer and wait for it."""
        return self.wait_ppm(self.shot(name), timeout)

    @staticmethod
    def wait_ppm(path: str, timeout: float = 10.0) -> Ppm:
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            if os.path.exists(path):
                try:
                    return Ppm(path)
                except (ValueError, OSError) as exc:  # still being written
                    last = exc
            time.sleep(0.02)
        raise TimeoutError("no framebuffer capture at %s (%s)" % (path, last))
