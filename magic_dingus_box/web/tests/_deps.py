"""Optional test dependencies: skip on a dev machine, FAIL in CI.

The production server legs (gunicorn gthread + the flask-sock WebSocket)
and the real-ffmpeg upload tests used to skip whenever their dependency
was missing — which is exactly what CI did for months, so the server the
boxes actually run was never exercised off a Pi and nothing showed it.
CI installs these deps (test-local.yml web-pytest); if one goes missing
again, the run goes red instead of quietly shrinking.

"In CI" = the CI environment variable is set to anything but an explicit
false (GitHub Actions sets CI=true).
"""
from __future__ import annotations

import importlib
import os
import shutil

import pytest


def in_ci() -> bool:
    return os.environ.get("CI", "").strip().lower() not in ("", "0", "false", "no")


def _missing(what: str) -> None:
    if in_ci():
        pytest.fail(f"{what} is missing in CI — install it in the workflow "
                    f"(test-local.yml web-pytest) instead of letting the test skip",
                    pytrace=False)
    pytest.skip(f"{what} not installed")


def need_module(*names: str) -> None:
    """Import each module or skip (dev machine) / fail (CI)."""
    for name in names:
        try:
            importlib.import_module(name)
        except ImportError:
            _missing(f"Python module {name!r}")


def need_tool(*names: str) -> None:
    """Find each executable on PATH or skip (dev machine) / fail (CI)."""
    for name in names:
        if shutil.which(name) is None:
            _missing(f"executable {name!r}")
