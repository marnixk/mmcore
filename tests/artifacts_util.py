"""Writable artifact directory for tests that save PNGs/PCAPs.

Defaults to ``/opt/cursor/artifacts`` (the Cloud Agent layout) and falls back
to a temp directory when that is not writable. Override with ``MMB_ARTIFACTS``.
"""

import os
import tempfile


def artifacts_dir() -> str:
    override = os.environ.get("MMB_ARTIFACTS")
    if override:
        os.makedirs(override, exist_ok=True)
        return override
    preferred = "/opt/cursor/artifacts"
    try:
        os.makedirs(preferred, exist_ok=True)
        return preferred
    except OSError:
        fallback = os.path.join(tempfile.gettempdir(), "mmbasic-artifacts")
        os.makedirs(fallback, exist_ok=True)
        return fallback


ARTIFACTS = artifacts_dir()
