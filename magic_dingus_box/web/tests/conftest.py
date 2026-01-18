"""
Pytest fixtures for OTA Update tests.
"""
from __future__ import annotations

import os
import json
import tempfile
from pathlib import Path
from typing import Generator
from unittest.mock import MagicMock, patch

import pytest

# Add parent directory to path for imports
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from admin import create_app


@pytest.fixture
def temp_data_dir() -> Generator[Path, None, None]:
    """Create a temporary data directory for testing."""
    # Save original tempfile settings
    original_tempdir = tempfile.tempdir
    original_tmpdir = os.environ.get("TMPDIR")

    with tempfile.TemporaryDirectory() as tmpdir:
        data_dir = Path(tmpdir) / "data"
        data_dir.mkdir(parents=True)

        # Create necessary subdirectories
        (data_dir / "playlists").mkdir()
        (data_dir / "media").mkdir()
        (data_dir / "roms").mkdir()
        (data_dir / "upload_temp").mkdir()  # Required by admin.py

        # Create a VERSION file in parent (simulating install dir)
        scripts_dir = data_dir.parent / "scripts"
        scripts_dir.mkdir(parents=True)

        version_file = data_dir.parent.parent / "VERSION"
        version_file.write_text("1.0.7\n")

        yield data_dir

    # Restore original tempfile settings after test
    tempfile.tempdir = original_tempdir
    if original_tmpdir is not None:
        os.environ["TMPDIR"] = original_tmpdir
    elif "TMPDIR" in os.environ:
        del os.environ["TMPDIR"]


@pytest.fixture
def app(temp_data_dir: Path):
    """Create Flask test application."""
    # Disable CSRF for testing
    os.environ["MAGIC_DISABLE_CSRF"] = "1"
    app = create_app(temp_data_dir, config={"TESTING": True})
    app.config["TESTING"] = True
    yield app
    # Clean up
    if "MAGIC_DISABLE_CSRF" in os.environ:
        del os.environ["MAGIC_DISABLE_CSRF"]


@pytest.fixture
def client(app):
    """Create test client."""
    return app.test_client()


@pytest.fixture
def mock_update_script(temp_data_dir: Path) -> Path:
    """Create a mock update script for testing."""
    scripts_dir = temp_data_dir.parent / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    script_path = scripts_dir / "update.sh"
    script_path.write_text('''#!/bin/bash
# Mock update script for testing

case "$1" in
    check)
        cat << 'EOF'
{
    "ok": true,
    "data": {
        "current_version": "1.0.7",
        "latest_version": "1.0.8",
        "update_available": true,
        "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz",
        "release_notes": "Bug fixes and improvements",
        "published_at": "2024-01-15T12:00:00Z",
        "has_backup": false
    }
}
EOF
        ;;
    install)
        echo '{"ok": true, "stage": "preparing", "progress": 5, "message": "Starting..."}'
        sleep 0.1
        echo '{"ok": true, "stage": "complete", "progress": 100, "message": "Update complete!", "new_version": "'"$2"'"}'
        ;;
    rollback)
        echo '{"ok": true, "stage": "complete", "progress": 100, "message": "Rollback complete!", "version": "1.0.6"}'
        ;;
    version)
        echo "1.0.7"
        ;;
esac
''')
    script_path.chmod(0o755)
    return script_path


@pytest.fixture
def mock_update_script_no_update(temp_data_dir: Path) -> Path:
    """Create a mock update script that reports no update available."""
    scripts_dir = temp_data_dir.parent / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    script_path = scripts_dir / "update.sh"
    script_path.write_text('''#!/bin/bash
# Mock update script - no update available

case "$1" in
    check)
        cat << 'EOF'
{
    "ok": true,
    "data": {
        "current_version": "1.0.8",
        "latest_version": "1.0.8",
        "update_available": false,
        "download_url": "",
        "release_notes": "",
        "published_at": "2024-01-15T12:00:00Z",
        "has_backup": true
    }
}
EOF
        ;;
esac
''')
    script_path.chmod(0o755)
    return script_path


@pytest.fixture
def mock_update_script_error(temp_data_dir: Path) -> Path:
    """Create a mock update script that simulates errors."""
    scripts_dir = temp_data_dir.parent / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    script_path = scripts_dir / "update.sh"
    script_path.write_text('''#!/bin/bash
# Mock update script - simulates errors

case "$1" in
    check)
        echo '{"ok": false, "error": {"message": "Failed to connect to GitHub"}}'
        exit 1
        ;;
    install)
        echo '{"ok": false, "error": {"message": "Download failed"}}'
        exit 1
        ;;
    rollback)
        echo '{"ok": false, "error": {"message": "No backup available for rollback"}}'
        exit 1
        ;;
esac
''')
    script_path.chmod(0o755)
    return script_path


@pytest.fixture
def mock_update_script_no_backup(temp_data_dir: Path) -> Path:
    """Create a mock update script that reports no backup for rollback."""
    scripts_dir = temp_data_dir.parent / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    script_path = scripts_dir / "update.sh"
    script_path.write_text('''#!/bin/bash
# Mock update script - no backup available

case "$1" in
    check)
        cat << 'EOF'
{
    "ok": true,
    "data": {
        "current_version": "1.0.7",
        "latest_version": "1.0.8",
        "update_available": true,
        "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz",
        "release_notes": "Bug fixes",
        "published_at": "2024-01-15T12:00:00Z",
        "has_backup": false
    }
}
EOF
        ;;
    rollback)
        echo '{"ok": false, "error": {"message": "No backup available for rollback"}}'
        exit 1
        ;;
esac
''')
    script_path.chmod(0o755)
    return script_path


@pytest.fixture
def csrf_token(client) -> str:
    """Get a CSRF token for protected endpoints."""
    # The admin API uses a session-based CSRF token
    # For testing, we can either disable CSRF or get a token from a GET endpoint
    # Since testing mode might have CSRF disabled, return a dummy token
    return "test-csrf-token"


@pytest.fixture
def auth_headers(csrf_token: str) -> dict:
    """Headers for authenticated requests."""
    return {
        "Content-Type": "application/json",
        "X-CSRF-Token": csrf_token
    }
