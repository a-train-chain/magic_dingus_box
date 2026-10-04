"""
OTA Update API Tests.

Tests cover the /admin/update/* endpoints in admin.py.
"""
from __future__ import annotations

import json
import time
from pathlib import Path
from unittest.mock import patch, MagicMock

import pytest


class TestCheckUpdate:
    """Tests for /admin/update/check endpoint."""

    def test_check_update_available(self, client, mock_update_script):
        """Test checking for updates when newer version is available."""
        response = client.get("/admin/update/check")

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert data["data"]["update_available"] is True
        assert data["data"]["current_version"] == "1.0.7"
        assert data["data"]["latest_version"] == "1.0.8"
        assert "github.com" in data["data"]["download_url"]

    def test_check_update_up_to_date(self, client, mock_update_script_no_update):
        """Test checking for updates when already at latest version."""
        response = client.get("/admin/update/check")

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert data["data"]["update_available"] is False
        assert data["data"]["current_version"] == "1.0.8"
        assert data["data"]["latest_version"] == "1.0.8"

    def test_check_update_github_error(self, client, mock_update_script_error):
        """Test that GitHub connection errors are handled gracefully."""
        response = client.get("/admin/update/check")

        # Should return error but not crash
        assert response.status_code == 500
        data = response.get_json()
        assert data["ok"] is False
        assert "error" in data

    def test_check_update_script_missing(self, client, temp_data_dir):
        """Test error when update script is not found."""
        # Remove the script if it exists
        script_path = temp_data_dir.parent / "scripts" / "update.sh"
        if script_path.exists():
            script_path.unlink()

        response = client.get("/admin/update/check")

        assert response.status_code == 500
        data = response.get_json()
        assert data["ok"] is False
        assert "UPDATE_NOT_AVAILABLE" in str(data.get("error", {}))


class TestInstallUpdate:
    """Tests for /admin/update/install endpoint."""

    def test_install_rejects_non_github_url(self, client, mock_update_script, auth_headers):
        """Test that non-GitHub URLs are rejected for security."""
        # Test with a malicious URL
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://evil.com/malware.tar.gz"
            },
            headers=auth_headers
        )

        assert response.status_code == 400
        data = response.get_json()
        assert data["ok"] is False
        assert "VALIDATION_ERROR" in str(data.get("error", {}))

    def test_install_accepts_github_url(self, client, mock_update_script, auth_headers):
        """Test that valid GitHub URLs are accepted."""
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz"
            },
            headers=auth_headers
        )

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert "job_id" in data.get("data", {})

    def test_install_accepts_api_github_url(self, client, mock_update_script, auth_headers):
        """Test that api.github.com URLs are also accepted."""
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://api.github.com/repos/a-train-chain/magic_dingus_box/tarball/v1.0.8"
            },
            headers=auth_headers
        )

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert "job_id" in data.get("data", {})

    def test_install_starts_background_job(self, client, mock_update_script, auth_headers):
        """Test that install starts a background job and returns job_id."""
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz"
            },
            headers=auth_headers
        )

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True

        job_id = data["data"]["job_id"]
        assert job_id is not None
        assert len(job_id) > 0

    def test_install_missing_version(self, client, mock_update_script, auth_headers):
        """Test that version is required."""
        response = client.post(
            "/admin/update/install",
            json={
                "download_url": "https://github.com/test/release.tar.gz"
            },
            headers=auth_headers
        )

        assert response.status_code == 400
        data = response.get_json()
        assert data["ok"] is False

    def test_install_missing_url(self, client, mock_update_script, auth_headers):
        """Test that download_url is required."""
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8"
            },
            headers=auth_headers
        )

        assert response.status_code == 400
        data = response.get_json()
        assert data["ok"] is False

    def test_install_missing_body(self, client, mock_update_script, auth_headers):
        """Test that JSON body is required."""
        response = client.post(
            "/admin/update/install",
            headers=auth_headers
        )

        assert response.status_code == 400


class TestJobStatus:
    """Tests for /admin/update/status/<job_id> endpoint."""

    def test_job_status_returns_progress(self, client, mock_update_script, auth_headers):
        """Test that job status endpoint returns progress info."""
        # First start an install job
        install_response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz"
            },
            headers=auth_headers
        )
        job_id = install_response.get_json()["data"]["job_id"]

        # Give the background thread a moment to process
        time.sleep(0.3)

        # Check job status
        response = client.get(f"/admin/update/status/{job_id}")

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert "status" in data["data"]
        assert "stage" in data["data"]
        assert "progress" in data["data"]
        assert "message" in data["data"]

    def test_job_status_not_found(self, client):
        """Test that nonexistent job returns 404."""
        response = client.get("/admin/update/status/nonexistent-job-id")

        assert response.status_code == 404
        data = response.get_json()
        assert data["ok"] is False

    def test_job_completes_successfully(self, client, mock_update_script, auth_headers):
        """Test that a job eventually completes."""
        # Start install
        install_response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz"
            },
            headers=auth_headers
        )
        job_id = install_response.get_json()["data"]["job_id"]

        # Poll until complete (with timeout)
        for _ in range(20):
            time.sleep(0.2)
            response = client.get(f"/admin/update/status/{job_id}")
            data = response.get_json()

            if data["data"]["status"] == "complete":
                assert data["data"]["stage"] == "complete"
                assert data["data"]["progress"] == 100
                return

        pytest.fail("Job did not complete within timeout")


class TestRollback:
    """Tests for /admin/update/rollback endpoint."""

    def test_rollback_no_backup_returns_error(self, client, mock_update_script_no_backup, auth_headers):
        """Test that rollback fails gracefully when no backup exists."""
        response = client.post(
            "/admin/update/rollback",
            headers=auth_headers
        )

        assert response.status_code == 500
        data = response.get_json()
        assert data["ok"] is False
        # Error could be "No backup" from script or "ROLLBACK_FAILED" from Flask
        error_info = str(data.get("error", {}))
        assert "backup" in error_info.lower() or "ROLLBACK_FAILED" in error_info

    def test_rollback_with_backup_succeeds(self, client, mock_update_script, auth_headers):
        """Test that rollback succeeds when backup exists."""
        # Modify the mock to simulate having a backup
        scripts_dir = Path(mock_update_script).parent
        script_path = scripts_dir / "update.sh"
        script_path.write_text('''#!/bin/bash
case "$1" in
    rollback)
        echo '{"ok": true, "stage": "complete", "progress": 100, "message": "Rollback complete!", "version": "1.0.6"}'
        ;;
esac
''')
        script_path.chmod(0o755)

        response = client.post(
            "/admin/update/rollback",
            headers=auth_headers
        )

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True


SLOW_UPDATE_SCRIPT = '''#!/bin/bash
case "$1" in
    install)
        echo '{"ok": true, "stage": "downloading", "progress": 10, "message": "Downloading..."}'
        sleep "${SLOW_SECONDS:-1.5}"
        echo '{"ok": true, "stage": "complete", "progress": 100, "message": "Update complete!", "new_version": "'"$2"'"}'
        ;;
    rollback)
        sleep "${SLOW_SECONDS:-1.5}"
        echo '{"ok": true, "stage": "complete", "progress": 100, "message": "Rollback complete!", "version": "1.0.6"}'
        ;;
esac
'''

GOOD_INSTALL = {
    "version": "1.0.8",
    "download_url": "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/release.tar.gz",
}


class TestDetachedOtaJobs:
    """The OTA job must outlive the web service and be reportable after it.

    magic-dingus-web.service is KillMode=control-group + Restart=always, so a
    restart kills every child — the old start_new_session=True did not help.
    Job state now lives on disk, so a restarted Flask answers status polls.
    """

    @pytest.fixture
    def slow_script(self, mock_update_script):
        mock_update_script.write_text(SLOW_UPDATE_SCRIPT)
        mock_update_script.chmod(0o755)
        return mock_update_script

    def _poll(self, client, job_id, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = client.get(f"/admin/update/status/{job_id}")
            assert r.status_code == 200, r.get_json()
            data = r.get_json()["data"]
            if data["status"] != "running":
                return data
            time.sleep(0.1)
        pytest.fail("job never left running")

    def test_status_survives_a_flask_restart(self, client, slow_script,
                                             temp_data_dir, auth_headers):
        from admin import create_app

        r = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r.status_code == 200
        job_id = r.get_json()["data"]["job_id"]

        # A brand-new app over the same data dir: no in-memory job at all.
        restarted = create_app(temp_data_dir, config={"TESTING": True})
        restarted.config["TESTING"] = True
        data = self._poll(restarted.test_client(), job_id)
        assert data["status"] == "complete"
        assert data["progress"] == 100
        assert data["new_version"] == "1.0.8"

    def test_second_install_while_one_runs_is_409(self, client, slow_script, auth_headers):
        r1 = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r1.status_code == 200
        r2 = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r2.status_code == 409
        assert "already running" in r2.get_json()["error"]["message"].lower()
        # And it frees up once the first one finishes.
        self._poll(client, r1.get_json()["data"]["job_id"])
        r3 = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r3.status_code == 200

    def test_rollback_while_install_runs_is_409(self, client, slow_script, auth_headers):
        r1 = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r1.status_code == 200
        r2 = client.post("/admin/update/rollback", headers=auth_headers)
        assert r2.status_code == 409

    def test_busy_is_also_seen_by_a_restarted_flask(self, client, slow_script,
                                                    temp_data_dir, auth_headers):
        from admin import create_app

        r1 = client.post("/admin/update/install", json=GOOD_INSTALL, headers=auth_headers)
        assert r1.status_code == 200
        restarted = create_app(temp_data_dir, config={"TESTING": True})
        restarted.config["TESTING"] = True
        r2 = restarted.test_client().post("/admin/update/install", json=GOOD_INSTALL,
                                          headers=auth_headers)
        assert r2.status_code == 409


class TestVersion:
    """Tests for /admin/update/version endpoint."""

    def test_get_version(self, client, mock_update_script):
        """Test getting current version."""
        response = client.get("/admin/update/version")

        assert response.status_code == 200
        data = response.get_json()
        assert data["ok"] is True
        assert "version" in data["data"]


class TestURLValidation:
    """Tests for URL validation security."""

    # The install endpoint pins download_url to THIS project's own repo
    # (a-train-chain/magic_dingus_box), not "any github.com URL" — a bare
    # github.com prefix let a LAN device install an attacker-owned tarball.
    @pytest.mark.parametrize("url,expected_valid", [
        # This project's own repo, at the tag named by `version` — accepted.
        # (The URL must name the SAME tag as the version being installed:
        # update.sh stamps VERSION from `version`, so a mismatched pair
        # would install one release while reporting another.)
        ("https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/file.tar.gz", True),
        ("https://api.github.com/repos/a-train-chain/magic_dingus_box/tarball/v1.0.8", True),
        ("https://codeload.github.com/a-train-chain/magic_dingus_box/tar.gz/refs/tags/v1.0.8", True),
        ("https://github.com/a-train-chain/magic_dingus_box/archive/refs/tags/v1.0.8.tar.gz", True),
        # Own repo, but a DIFFERENT tag than `version` — rejected.
        ("https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.0/file.tar.gz", False),
        # Own repo, but not a tagged release artifact (branch archive, bare
        # repo page) — rejected: only tagged releases are installable.
        ("https://github.com/a-train-chain/magic_dingus_box/archive/main.tar.gz", False),
        ("https://codeload.github.com/a-train-chain/magic_dingus_box/tar.gz/refs/heads/main", False),
        ("https://github.com/a-train-chain/magic_dingus_box", False),
        # Query strings / encoded separators in the asset name — rejected.
        ("https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/file.tar.gz?x=1", False),
        ("https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/..%2F..%2Fx.tar.gz", False),
        # A DIFFERENT GitHub repo — now rejected (was the RCE hole).
        ("https://github.com/user/repo/releases/download/v1.0.0/file.tar.gz", False),
        ("https://api.github.com/repos/attacker/evil/tarball/v1.0.0", False),
        ("https://evil.com/malware.tar.gz", False),
        ("http://github.com/a-train-chain/magic_dingus_box/file.tar.gz", False),  # http not https
        ("https://github.com.evil.com/file.tar.gz", False),  # subdomain trick
        ("https://not-github.com/file.tar.gz", False),
        ("file:///etc/passwd", False),
        ("javascript:alert(1)", False),
        # Dot-segment normalization bypass: passes a naive startswith() but
        # curl -L normalizes /../ before the request → attacker/repo. Must
        # be rejected by comparing the NORMALIZED path.
        ("https://github.com/a-train-chain/magic_dingus_box/../../attacker/evil/archive/main.tar.gz", False),
        # Sibling repo whose name merely starts with the pinned repo.
        ("https://github.com/a-train-chain/magic_dingus_box_evil/releases/download/v1/f.tar.gz", False),
    ])
    def test_url_validation(self, client, mock_update_script, auth_headers, url, expected_valid):
        """Test URL validation with various inputs."""
        response = client.post(
            "/admin/update/install",
            json={
                "version": "1.0.8",
                "download_url": url
            },
            headers=auth_headers
        )

        if expected_valid:
            assert response.status_code == 200
        else:
            assert response.status_code == 400


class TestVersionValidation:
    """`version` is NOT an inert label: update.sh interpolates it into
    https://api.github.com/repos/<repo>/releases/tags/v${version} to find
    the pre-compiled binary, and curl normalizes dot-segments — so a
    version like "1.0.8/../../../../attacker/evil/releases/tags/v1" made the
    Pi fetch ANOTHER repo's release metadata and install its binary, while
    download_url still pointed at a perfectly valid asset of our own repo.
    It is also written verbatim into VERSION. Only plain X.Y.Z is accepted.
    """

    GOOD_URL = ("https://github.com/a-train-chain/magic_dingus_box/"
                "releases/download/v1.0.8/release.tar.gz")

    @pytest.mark.parametrize("version", [
        "1.0.8/../../../../attacker/evil/releases/tags/v1",
        "../../attacker/evil",
        "1.0.8\n",
        "1.0.8 ",
        "v1.0.8",
        "1.0",
        "1.0.8-rc1",
        "1.0.8;rm -rf /",
        "١.0.8",  # Arabic-Indic digit: \d would accept it, [0-9] must not
        "",
    ])
    def test_install_rejects_malformed_version(self, client, mock_update_script,
                                               auth_headers, version):
        response = client.post(
            "/admin/update/install",
            json={"version": version, "download_url": self.GOOD_URL},
            headers=auth_headers,
        )
        assert response.status_code == 400
        assert response.get_json()["ok"] is False

    @pytest.mark.parametrize("version", [123, ["1.0.8"], {"v": "1.0.8"}])
    def test_install_rejects_non_string_version(self, client, mock_update_script,
                                                auth_headers, version):
        response = client.post(
            "/admin/update/install",
            json={"version": version, "download_url": self.GOOD_URL},
            headers=auth_headers,
        )
        assert response.status_code == 400

    def test_install_accepts_plain_semver(self, client, mock_update_script, auth_headers):
        response = client.post(
            "/admin/update/install",
            json={"version": "1.0.8", "download_url": self.GOOD_URL},
            headers=auth_headers,
        )
        assert response.status_code == 200
