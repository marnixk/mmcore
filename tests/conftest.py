import fcntl
import os
import re
import subprocess

import pytest

from harness import MMBasicConsole, qemu_usb_net_args

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO_ROOT, "console", "kernel8.img")
BUILD_LOCK = os.path.join(REPO_ROOT, ".pytest-build.lock")

BUILD_INPUT_DIRS = (
    "mmbasic",
    "console",
    "scripts",
    "patches",
    "ramdisk",
)

# The kernel bakes the build-time `git describe` string into this banner
# literal, so the image itself records the version it was built for.
_BAKED_VERSION_RE = re.compile(
    rb"mmcore operating system - (.*?) - 2026 \(c\) Marnix Kok"
)


def _git_describe() -> str:
    """The version the source tree is currently at (``git describe``)."""
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always"],
            cwd=REPO_ROOT,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _baked_version(image: str = KERNEL) -> str | None:
    """Version string embedded in a built kernel image, or ``None``."""
    try:
        with open(image, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    match = _BAKED_VERSION_RE.search(data)
    return match.group(1).decode("utf-8", "replace") if match else None


def _needs_version_rebuild(baked: str | None, described: str) -> bool:
    """Whether the image's baked version no longer matches the tree.

    A commit that touches no build input (docs/tests only) changes HEAD and
    therefore ``git describe``, but leaves every mtime untouched. Comparing the
    version actually embedded in the image against the current one catches
    that without rebuilding on every run.
    """
    if not described:
        return False
    if baked is None:
        # No version literal found: never trust an unrecognised image.
        return True
    return baked != described


def _kernel_is_stale(image: str = KERNEL) -> bool:
    if not os.path.isfile(image):
        return True
    if os.path.getmtime(image) < _newest_source_mtime():
        return True
    # An explicit MMB_VERSION (release packaging) is not `git describe`, so do
    # not second-guess a deliberately versioned image.
    if os.environ.get("MMB_VERSION"):
        return False
    return _needs_version_rebuild(_baked_version(image), _git_describe())


def _newest_source_mtime() -> float:
    newest = 0.0
    for name in BUILD_INPUT_DIRS:
        root = os.path.join(REPO_ROOT, name)
        for base, dirs, files in os.walk(root):
            if "__pycache__" in base:
                continue
            for f in files:
                if f.endswith((".o", ".d", ".a")):
                    continue
                try:
                    newest = max(newest, os.path.getmtime(os.path.join(base, f)))
                except OSError:
                    pass
    return newest


@pytest.fixture(scope="session")
def kernel_image() -> str:
    """Ensure the bare-metal console image is built (idempotent).

    Serialised with a lock so parallel pytest workers do not race inside
    ``scripts/build.sh``; the build is skipped when the image is newer than
    every source it depends on.
    """
    with open(BUILD_LOCK, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            if _kernel_is_stale():
                subprocess.run(
                    ["bash", os.path.join(REPO_ROOT, "scripts", "build.sh")],
                    check=True,
                    cwd=REPO_ROOT,
                )
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    assert os.path.isfile(KERNEL), "build did not produce console/kernel8.img"
    return KERNEL


def _qemu_exclusive(request) -> bool:
    """Whether a test/module asks for the QEMU lane to itself.

    Timing-sensitive modules mark themselves ``qemu_exclusive`` so no other
    QEMU instance competes for CPU while they run (see the lane lock in
    ``harness/qemu_harness.py``).
    """
    return request.node.get_closest_marker("qemu_exclusive") is not None


@pytest.fixture(scope="module")
def console(kernel_image: str, request):
    """A booted QEMU console shared across a module's tests."""
    con = MMBasicConsole(kernel_image, exclusive=_qemu_exclusive(request))
    con.start()
    yield con
    con.stop()


@pytest.fixture
def fresh_console(kernel_image: str, request):
    """A freshly booted console per test (clean screen for graphics tests)."""
    con = MMBasicConsole(kernel_image, exclusive=_qemu_exclusive(request))
    con.start()
    yield con
    con.stop()


@pytest.fixture(scope="module")
def net_console(kernel_image: str, request):
    """Booted QEMU with USB CDC Ethernet (SLIRP user net). Not the default."""
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(),
        boot_timeout=40.0,
        exclusive=_qemu_exclusive(request),
    )
    con.start()
    yield con
    con.stop()
