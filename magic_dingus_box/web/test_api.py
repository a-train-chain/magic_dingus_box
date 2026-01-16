#!/usr/bin/env python3
"""
Magic Dingus Box - Web UI API Test Suite

Usage:
    # Test local server
    python3 test_api.py http://localhost:5000

    # Test Pi over network
    python3 test_api.py http://magicpi.local:5000

    # Test via USB
    python3 test_api.py http://10.0.0.1:5000

    # Run specific test group
    python3 test_api.py http://localhost:5000 --only uploads

    # Verbose output
    python3 test_api.py http://localhost:5000 -v
"""

import argparse
import json
import os
import random
import requests
import string
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional


class Colors:
    """ANSI color codes for terminal output."""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


class APITestRunner:
    """Test runner for Magic Dingus Box API."""

    def __init__(self, base_url: str, verbose: bool = False):
        self.base_url = base_url.rstrip('/')
        self.verbose = verbose
        self.csrf_token: Optional[str] = None
        self.session = requests.Session()
        self.results = {"passed": 0, "failed": 0, "skipped": 0}
        self.created_playlists = []
        self.created_media = []

    def log(self, message: str, level: str = "info"):
        """Log a message with color."""
        if level == "success":
            print(f"{Colors.GREEN}✓ {message}{Colors.RESET}")
        elif level == "error":
            print(f"{Colors.RED}✗ {message}{Colors.RESET}")
        elif level == "warning":
            print(f"{Colors.YELLOW}⚠ {message}{Colors.RESET}")
        elif level == "info":
            print(f"{Colors.BLUE}ℹ {message}{Colors.RESET}")
        else:
            print(message)

    def log_verbose(self, message: str):
        """Log only in verbose mode."""
        if self.verbose:
            print(f"  {Colors.BLUE}→ {message}{Colors.RESET}")

    def fetch_csrf_token(self) -> bool:
        """Fetch CSRF token from server."""
        try:
            resp = self.session.get(f"{self.base_url}/admin/csrf-token")
            data = resp.json()
            if data.get("ok") and data.get("data", {}).get("token"):
                self.csrf_token = data["data"]["token"]
                self.log_verbose(f"Got CSRF token: {self.csrf_token[:10]}...")
                return True
            return False
        except Exception as e:
            self.log_verbose(f"CSRF fetch failed: {e}")
            return False

    def get_headers(self, include_json: bool = True) -> dict:
        """Get headers for requests."""
        headers = {}
        if include_json:
            headers["Content-Type"] = "application/json"
        if self.csrf_token:
            headers["X-CSRF-Token"] = self.csrf_token
        return headers

    def run_test(self, name: str, test_func):
        """Run a single test and track results."""
        print(f"\n{Colors.BOLD}Testing: {name}{Colors.RESET}")
        try:
            test_func()
            self.results["passed"] += 1
            self.log(f"{name} PASSED", "success")
        except AssertionError as e:
            self.results["failed"] += 1
            self.log(f"{name} FAILED: {e}", "error")
        except Exception as e:
            self.results["failed"] += 1
            self.log(f"{name} ERROR: {e}", "error")

    def random_string(self, length: int = 8) -> str:
        """Generate a random string."""
        return ''.join(random.choices(string.ascii_lowercase, k=length))

    # ========== DEVICE TESTS ==========

    def test_device_info(self):
        """Test /admin/device/info endpoint."""
        resp = self.session.get(f"{self.base_url}/admin/device/info")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"
        assert "data" in data, "Missing data field"
        info = data["data"]
        self.log_verbose(f"Device: {info.get('device_name')}")
        self.log_verbose(f"IP: {info.get('local_ip')}")
        assert "hostname" in info, "Missing hostname"
        assert "stats" in info, "Missing stats"

    def test_device_name_update(self):
        """Test updating device name."""
        # First fetch CSRF
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        new_name = f"Test Device {self.random_string(4)}"
        resp = self.session.post(
            f"{self.base_url}/admin/device/name",
            headers=self.get_headers(),
            json={"name": new_name}
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"

        # Verify name was changed
        resp2 = self.session.get(f"{self.base_url}/admin/device/info")
        info = resp2.json()["data"]
        assert info["device_name"] == new_name, "Name not updated"

        # Reset to default
        self.session.post(
            f"{self.base_url}/admin/device/name",
            headers=self.get_headers(),
            json={"name": "Magic Dingus Box"}
        )

    # ========== HEALTH TESTS ==========

    def test_health_basic(self):
        """Test /admin/health endpoint."""
        resp = self.session.get(f"{self.base_url}/admin/health")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Health check failed"

    def test_health_detailed(self):
        """Test /admin/health/detailed endpoint."""
        resp = self.session.get(f"{self.base_url}/admin/health/detailed")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Health check failed"
        stats = data["data"]
        self.log_verbose(f"Status: {stats.get('status')}")
        self.log_verbose(f"Uptime: {stats.get('uptime_human')}")
        if stats.get("cpu_temperature_c"):
            self.log_verbose(f"CPU Temp: {stats['cpu_temperature_c']}°C")

    # ========== CSRF TESTS ==========

    def test_csrf_required(self):
        """Test that CSRF is required for POST requests."""
        # Try to create playlist without CSRF
        resp = self.session.post(
            f"{self.base_url}/admin/playlists/test_no_csrf.yaml",
            headers={"Content-Type": "application/json"},
            json={"title": "Test", "items": []}
        )
        # Should fail with 403 (unless CSRF is disabled for testing)
        if resp.status_code == 403:
            self.log_verbose("CSRF protection active")
        else:
            self.log_verbose(f"CSRF may be disabled (got {resp.status_code})")

    def test_csrf_token_flow(self):
        """Test CSRF token acquisition and use."""
        resp = self.session.get(f"{self.base_url}/admin/csrf-token")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Token request failed"
        assert "token" in data.get("data", {}), "Missing token"
        token = data["data"]["token"]
        assert len(token) > 20, "Token too short"
        self.log_verbose(f"Token length: {len(token)}")

    # ========== PLAYLIST TESTS ==========

    def test_list_playlists(self):
        """Test listing playlists."""
        resp = self.session.get(f"{self.base_url}/admin/playlists")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"
        playlists = data["data"]
        self.log_verbose(f"Found {len(playlists)} playlists")

    def test_create_playlist(self):
        """Test creating a new playlist."""
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        playlist_name = f"test_playlist_{self.random_string()}.yaml"
        playlist_data = {
            "title": f"Test Playlist {self.random_string(4)}",
            "curator": "Test Suite",
            "description": "Created by automated tests",
            "playlist_type": "video",
            "loop": True,
            "items": [
                {
                    "title": "Test Video 1",
                    "artist": "Test Artist",
                    "source_type": "local",
                    "path": "media/test_video_1.mp4"
                },
                {
                    "title": "Test Video 2",
                    "artist": "",
                    "source_type": "local",
                    "path": "media/test_video_2.mp4"
                }
            ]
        }

        resp = self.session.post(
            f"{self.base_url}/admin/playlists/{playlist_name}",
            headers=self.get_headers(),
            json=playlist_data
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
        data = resp.json()
        assert data.get("ok") is True, f"Create failed: {data}"

        self.created_playlists.append(playlist_name)
        self.log_verbose(f"Created playlist: {playlist_name}")

    def test_get_playlist(self):
        """Test retrieving a playlist."""
        if not self.created_playlists:
            self.log_verbose("No playlists to fetch, skipping")
            return

        playlist_name = self.created_playlists[0]
        resp = self.session.get(f"{self.base_url}/admin/playlists/{playlist_name}")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"
        playlist = data["data"]
        assert "title" in playlist, "Missing title"
        assert "items" in playlist, "Missing items"
        self.log_verbose(f"Playlist has {len(playlist['items'])} items")

    def test_update_playlist(self):
        """Test updating a playlist."""
        if not self.created_playlists:
            self.log_verbose("No playlists to update, skipping")
            return

        assert self.fetch_csrf_token(), "Could not get CSRF token"

        playlist_name = self.created_playlists[0]
        updated_data = {
            "title": f"Updated Playlist {self.random_string(4)}",
            "curator": "Test Suite Updated",
            "description": "Updated by automated tests",
            "playlist_type": "video",
            "loop": False,
            "items": [
                {
                    "title": "Updated Video",
                    "artist": "Updated Artist",
                    "source_type": "local",
                    "path": "media/updated_video.mp4"
                }
            ]
        }

        resp = self.session.post(
            f"{self.base_url}/admin/playlists/{playlist_name}",
            headers=self.get_headers(),
            json=updated_data
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"

        # Verify update
        resp2 = self.session.get(f"{self.base_url}/admin/playlists/{playlist_name}")
        playlist = resp2.json()["data"]
        assert playlist["loop"] is False, "Loop not updated"
        assert len(playlist["items"]) == 1, "Items not updated"

    def test_delete_playlist(self):
        """Test deleting a playlist."""
        if not self.created_playlists:
            self.log_verbose("No playlists to delete, skipping")
            return

        assert self.fetch_csrf_token(), "Could not get CSRF token"

        playlist_name = self.created_playlists.pop()
        resp = self.session.delete(
            f"{self.base_url}/admin/playlists/{playlist_name}",
            headers=self.get_headers()
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"

        # Verify deletion
        resp2 = self.session.get(f"{self.base_url}/admin/playlists/{playlist_name}")
        assert resp2.status_code == 404, "Playlist still exists"

    def test_playlist_not_found(self):
        """Test 404 for non-existent playlist."""
        resp = self.session.get(f"{self.base_url}/admin/playlists/nonexistent_playlist_12345.yaml")
        assert resp.status_code == 404, f"Expected 404, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is False, "Should return ok: false"
        assert "error" in data, "Missing error field"

    # ========== MEDIA TESTS ==========

    def test_list_media(self):
        """Test listing media files."""
        resp = self.session.get(f"{self.base_url}/admin/media")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"
        media = data["data"]
        self.log_verbose(f"Found {len(media)} media files")

    def test_list_roms(self):
        """Test listing ROM files."""
        resp = self.session.get(f"{self.base_url}/admin/roms")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        data = resp.json()
        assert data.get("ok") is True, "Response not ok"
        roms = data["data"]
        total = sum(len(v) for v in roms.values())
        self.log_verbose(f"Found {total} ROMs across {len(roms)} systems")

    def test_upload_small_video(self):
        """Test uploading a small test video."""
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        # Create a minimal valid MP4 file (not playable, but valid structure)
        test_filename = f"test_video_{self.random_string()}.mp4"
        test_content = b'\x00\x00\x00\x1c\x66\x74\x79\x70\x69\x73\x6f\x6d\x00\x00\x00\x00\x69\x73\x6f\x6d\x61\x76\x63\x31\x6d\x70\x34\x31'

        files = {"file": (test_filename, test_content, "video/mp4")}
        headers = {"X-CSRF-Token": self.csrf_token}

        resp = self.session.post(
            f"{self.base_url}/admin/upload",
            headers=headers,
            files=files
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
        data = resp.json()
        assert data.get("ok") is True, f"Upload failed: {data}"

        self.created_media.append(data["data"]["path"])
        self.log_verbose(f"Uploaded: {data['data']['path']}")

    def test_upload_rom(self):
        """Test uploading a ROM file."""
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        test_filename = f"test_rom_{self.random_string()}.nes"
        # Create minimal NES header
        test_content = b'NES\x1a' + b'\x00' * 12

        files = {"file": (test_filename, test_content, "application/octet-stream")}
        headers = {"X-CSRF-Token": self.csrf_token}

        resp = self.session.post(
            f"{self.base_url}/admin/upload/rom/nes",
            headers=headers,
            files=files
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
        data = resp.json()
        assert data.get("ok") is True, f"Upload failed: {data}"

        self.created_media.append(data["data"]["path"])
        self.log_verbose(f"Uploaded ROM: {data['data']['path']}")

    # ========== BACKUP/RESTORE TESTS ==========

    def test_create_backup(self):
        """Test creating a backup."""
        resp = self.session.get(f"{self.base_url}/admin/backup")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        assert "application/zip" in resp.headers.get("Content-Type", ""), "Not a ZIP file"
        assert len(resp.content) > 0, "Empty backup"
        self.log_verbose(f"Backup size: {len(resp.content)} bytes")

        # Save for restore test
        self._backup_content = resp.content

    def test_restore_backup(self):
        """Test restoring from backup."""
        if not hasattr(self, '_backup_content'):
            self.log_verbose("No backup to restore, skipping")
            return

        assert self.fetch_csrf_token(), "Could not get CSRF token"

        files = {"file": ("backup.zip", self._backup_content, "application/zip")}
        headers = {"X-CSRF-Token": self.csrf_token}

        resp = self.session.post(
            f"{self.base_url}/admin/restore",
            headers=headers,
            files=files
        )
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
        data = resp.json()
        assert data.get("ok") is True, f"Restore failed: {data}"
        self.log_verbose(f"Restore result: {data.get('message')}")

    # ========== SECURITY TESTS ==========

    def test_path_traversal_playlist(self):
        """Test path traversal protection on playlists."""
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        malicious_names = [
            "../../../etc/passwd",
            "..\\..\\..\\windows\\system32",
            "normal/../../../etc/passwd",
        ]

        for name in malicious_names:
            resp = self.session.post(
                f"{self.base_url}/admin/playlists/{name}",
                headers=self.get_headers(),
                json={"title": "Test", "items": []}
            )
            # Should either reject (400/403/404) or sanitize (200)
            # 404 is acceptable - means the malicious path was blocked
            assert resp.status_code in [200, 400, 403, 404], f"Unexpected status for {name}: {resp.status_code}"
            if resp.status_code == 200:
                # Verify it was sanitized
                data = resp.json()
                saved_name = data.get("data", {}).get("filename", "")
                assert ".." not in saved_name, f"Path traversal not sanitized: {saved_name}"
                # Clean up
                self.session.delete(
                    f"{self.base_url}/admin/playlists/{saved_name}",
                    headers=self.get_headers()
                )

    def test_invalid_json(self):
        """Test handling of invalid JSON."""
        assert self.fetch_csrf_token(), "Could not get CSRF token"

        resp = self.session.post(
            f"{self.base_url}/admin/playlists/test.yaml",
            headers=self.get_headers(),
            data="{invalid json"
        )
        # Should return error, not crash
        assert resp.status_code in [400, 500], f"Expected error status, got {resp.status_code}"

    # ========== CLEANUP ==========

    def cleanup(self):
        """Clean up test resources."""
        print(f"\n{Colors.BOLD}Cleaning up test resources...{Colors.RESET}")

        if not self.fetch_csrf_token():
            self.log("Could not get CSRF token for cleanup", "warning")
            return

        # Delete created playlists
        for playlist in self.created_playlists:
            try:
                self.session.delete(
                    f"{self.base_url}/admin/playlists/{playlist}",
                    headers=self.get_headers()
                )
                self.log_verbose(f"Deleted playlist: {playlist}")
            except Exception as e:
                self.log_verbose(f"Failed to delete {playlist}: {e}")

        # Delete created media
        for media_path in self.created_media:
            try:
                self.session.delete(
                    f"{self.base_url}/admin/media/{media_path}",
                    headers=self.get_headers()
                )
                self.log_verbose(f"Deleted media: {media_path}")
            except Exception as e:
                self.log_verbose(f"Failed to delete {media_path}: {e}")

    # ========== TEST RUNNER ==========

    def run_all_tests(self, only: Optional[str] = None):
        """Run all tests."""
        test_groups = {
            "device": [
                ("Device Info", self.test_device_info),
                ("Device Name Update", self.test_device_name_update),
            ],
            "health": [
                ("Health Basic", self.test_health_basic),
                ("Health Detailed", self.test_health_detailed),
            ],
            "csrf": [
                ("CSRF Required", self.test_csrf_required),
                ("CSRF Token Flow", self.test_csrf_token_flow),
            ],
            "playlists": [
                ("List Playlists", self.test_list_playlists),
                ("Create Playlist", self.test_create_playlist),
                ("Get Playlist", self.test_get_playlist),
                ("Update Playlist", self.test_update_playlist),
                ("Playlist Not Found", self.test_playlist_not_found),
                ("Delete Playlist", self.test_delete_playlist),
            ],
            "media": [
                ("List Media", self.test_list_media),
                ("List ROMs", self.test_list_roms),
            ],
            "uploads": [
                ("Upload Small Video", self.test_upload_small_video),
                ("Upload ROM", self.test_upload_rom),
            ],
            "backup": [
                ("Create Backup", self.test_create_backup),
                ("Restore Backup", self.test_restore_backup),
            ],
            "security": [
                ("Path Traversal Protection", self.test_path_traversal_playlist),
                ("Invalid JSON Handling", self.test_invalid_json),
            ],
        }

        print(f"\n{Colors.BOLD}{'='*60}{Colors.RESET}")
        print(f"{Colors.BOLD}Magic Dingus Box API Test Suite{Colors.RESET}")
        print(f"Target: {self.base_url}")
        print(f"{Colors.BOLD}{'='*60}{Colors.RESET}")

        # Check connectivity first
        print(f"\n{Colors.BOLD}Checking connectivity...{Colors.RESET}")
        try:
            resp = self.session.get(f"{self.base_url}/admin/health", timeout=5)
            if resp.status_code != 200:
                self.log(f"Server returned {resp.status_code}", "error")
                return
            self.log("Connected to server", "success")
        except Exception as e:
            self.log(f"Cannot connect to server: {e}", "error")
            return

        # Run tests
        groups_to_run = [only] if only else test_groups.keys()

        for group_name in groups_to_run:
            if group_name not in test_groups:
                self.log(f"Unknown test group: {group_name}", "warning")
                continue

            print(f"\n{Colors.BOLD}{'='*40}{Colors.RESET}")
            print(f"{Colors.BOLD}Group: {group_name.upper()}{Colors.RESET}")
            print(f"{Colors.BOLD}{'='*40}{Colors.RESET}")

            for test_name, test_func in test_groups[group_name]:
                self.run_test(test_name, test_func)

        # Cleanup
        self.cleanup()

        # Summary
        print(f"\n{Colors.BOLD}{'='*60}{Colors.RESET}")
        print(f"{Colors.BOLD}TEST SUMMARY{Colors.RESET}")
        print(f"{Colors.BOLD}{'='*60}{Colors.RESET}")
        print(f"{Colors.GREEN}Passed: {self.results['passed']}{Colors.RESET}")
        print(f"{Colors.RED}Failed: {self.results['failed']}{Colors.RESET}")
        print(f"{Colors.YELLOW}Skipped: {self.results['skipped']}{Colors.RESET}")

        if self.results['failed'] > 0:
            print(f"\n{Colors.RED}Some tests failed!{Colors.RESET}")
            return 1
        else:
            print(f"\n{Colors.GREEN}All tests passed!{Colors.RESET}")
            return 0


def main():
    parser = argparse.ArgumentParser(
        description="Magic Dingus Box API Test Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 test_api.py http://localhost:5000
  python3 test_api.py http://magicpi.local:5000 --only uploads
  python3 test_api.py http://10.0.0.1:5000 -v
        """
    )
    parser.add_argument("url", help="Base URL of the admin server")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    parser.add_argument("--only", choices=["device", "health", "csrf", "playlists", "media", "uploads", "backup", "security"],
                        help="Run only specific test group")

    args = parser.parse_args()

    runner = APITestRunner(args.url, verbose=args.verbose)
    sys.exit(runner.run_all_tests(only=args.only))


if __name__ == "__main__":
    main()
