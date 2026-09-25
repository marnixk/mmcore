"""Decide whether ``console/kernel8.img`` is stale for the current checkout.

The image bakes in ``MMB_VERSION`` from ``git describe --tags --always``
(#732). A pure mtime check misses a commit or tag that changes that string
without touching a build input, so an existing image can report an old version
and desync the startup/credits tests. Fold the version recorded in
``console/mmb_version.h`` into the staleness decision.
"""

import os
import re
import subprocess

_VERSION_RE = re.compile(r'#define\s+MMB_VERSION\s+"([^"]*)"')


def read_built_version(header_path):
    """Return the MMB_VERSION recorded in a generated header, or ``""``."""
    try:
        with open(header_path, encoding="utf-8") as fh:
            match = _VERSION_RE.search(fh.read())
    except OSError:
        return ""
    return match.group(1) if match else ""


def git_version(repo_root):
    """Return ``git describe --tags --always`` for *repo_root*, or ``""``."""
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always"],
            cwd=repo_root,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def is_stale(kernel_path, header_path, current_version, newest_source_mtime):
    """True if *kernel_path* must be rebuilt for the current checkout."""
    if not os.path.isfile(kernel_path):
        return True
    if os.path.getmtime(kernel_path) < newest_source_mtime:
        return True
    built = read_built_version(header_path)
    return built != (current_version or "dev")
