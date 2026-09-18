import fcntl
import os
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
)


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
            stale = not os.path.isfile(KERNEL)
            if not stale:
                stale = os.path.getmtime(KERNEL) < _newest_source_mtime()
            if stale:
                subprocess.run(
                    ["bash", os.path.join(REPO_ROOT, "scripts", "build.sh")],
                    check=True,
                    cwd=REPO_ROOT,
                )
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    assert os.path.isfile(KERNEL), "build did not produce console/kernel8.img"
    return KERNEL


@pytest.fixture(scope="module")
def console(kernel_image: str):
    """A booted QEMU console shared across a module's tests."""
    con = MMBasicConsole(kernel_image)
    con.start()
    yield con
    con.stop()


@pytest.fixture
def fresh_console(kernel_image: str):
    """A freshly booted console per test (clean screen for graphics tests)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    yield con
    con.stop()


@pytest.fixture(scope="module")
def net_console(kernel_image: str):
    """Booted QEMU with USB CDC Ethernet (SLIRP user net). Not the default."""
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(),
        boot_timeout=40.0,
    )
    con.start()
    yield con
    con.stop()
