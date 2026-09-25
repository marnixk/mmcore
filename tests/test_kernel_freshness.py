"""Regression tests for #732.

The kernel-image staleness check must notice a version change (a commit or
tag that alters ``git describe``) even when no build input's mtime moved, so
the embedded banner version cannot desync from the test's expectation.
"""

import os
import time

import kernel_freshness


def _write_header(path, version):
    path.write_text(
        "#ifndef MMB_VERSION_H\n"
        "#define MMB_VERSION_H\n"
        f'#define MMB_VERSION "{version}"\n'
        "#endif\n"
    )


def test_read_built_version(tmp_path):
    header = tmp_path / "mmb_version.h"
    _write_header(header, "v1.2.3-4-gabcdef")
    assert kernel_freshness.read_built_version(str(header)) == "v1.2.3-4-gabcdef"


def test_read_built_version_missing(tmp_path):
    assert kernel_freshness.read_built_version(str(tmp_path / "nope.h")) == ""


def test_read_built_version_unparseable(tmp_path):
    header = tmp_path / "mmb_version.h"
    header.write_text("#define MMB_VERSION_NO 1\n")
    assert kernel_freshness.read_built_version(str(header)) == ""


def test_missing_kernel_is_stale(tmp_path):
    assert kernel_freshness.is_stale(
        str(tmp_path / "kernel8.img"), str(tmp_path / "mmb_version.h"), "v1", 0.0
    )


def test_newer_source_is_stale(tmp_path):
    kernel = tmp_path / "kernel8.img"
    header = tmp_path / "mmb_version.h"
    kernel.write_bytes(b"")
    _write_header(header, "v1")
    old = time.time() - 100
    os.utime(kernel, (old, old))
    assert kernel_freshness.is_stale(str(kernel), str(header), "v1", old + 10)


def test_version_change_is_stale(tmp_path):
    kernel = tmp_path / "kernel8.img"
    header = tmp_path / "mmb_version.h"
    kernel.write_bytes(b"")
    _write_header(header, "v1.0.0-3-gabc1234")
    assert kernel_freshness.is_stale(
        str(kernel), str(header), "v1.0.0-4-gdef5678", 0.0
    )


def test_matching_version_is_fresh(tmp_path):
    kernel = tmp_path / "kernel8.img"
    header = tmp_path / "mmb_version.h"
    kernel.write_bytes(b"")
    _write_header(header, "v1.0.0-4-gdef5678")
    assert not kernel_freshness.is_stale(
        str(kernel), str(header), "v1.0.0-4-gdef5678", 0.0
    )


def test_no_git_falls_back_to_dev(tmp_path):
    kernel = tmp_path / "kernel8.img"
    header = tmp_path / "mmb_version.h"
    kernel.write_bytes(b"")
    _write_header(header, "dev")
    assert not kernel_freshness.is_stale(str(kernel), str(header), "", 0.0)
