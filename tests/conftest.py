import os
import subprocess

import pytest

from harness import MMBasicConsole, qemu_usb_net_args

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO_ROOT, "console", "kernel8.img")


@pytest.fixture(scope="session")
def kernel_image() -> str:
    """Ensure the bare-metal console image is built (idempotent)."""
    subprocess.run(
        ["bash", os.path.join(REPO_ROOT, "scripts", "build.sh")],
        check=True,
        cwd=REPO_ROOT,
    )
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
