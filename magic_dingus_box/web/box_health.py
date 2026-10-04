"""Box health: run the box's own acceptance test and keep its last verdict.

verify_box.sh is the single "is this box shippable?" command (see CLAUDE.md
"Pre-ship acceptance test"). The Content Manager's Box health card runs it
so an owner — or support, over the phone — can see the same verdict a
technician would get over SSH, without SSH.

* The command is FIXED: `sudo -n /bin/bash <install>/scripts/verify_box.sh`
  plus an optional `--with-services`. No request value reaches the argv.
  sudo because several checks need root (docker ps, cgroup files, other
  users' processes); the script re-derives HOME from SUDO_USER, so the
  per-user paths (cores, BIOS) still resolve to the web user's.
* Single-flight, in-process: a run is ~30 s; a second request while one is
  running gets the running one's status instead of a second root process.
  The runner thread lives in the one gunicorn worker (see serve.py).
* The last result is cached as JSON in the data dir so the card has
  something to show after a page reload or web restart. The OTA's
  rsync --delete removes it on update, deliberately: a verdict describes
  the software that produced it.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Optional

try:
    from redact import Redactor
except ImportError:  # pragma: no cover - package form
    from .redact import Redactor

CACHE_NAME = "box_health_last.json"
RUN_TIMEOUT_S = 180
RUN_TIMEOUT_WITH_SERVICES_S = 600
MAX_OUTPUT_CHARS = 200_000

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_SECTION_RE = re.compile(r"^\s*==\s*(.+?)\s*==\s*$")
_CHECK_RE = re.compile(r"^\s*\[(PASS|FAIL|WARN)\]\s?(.*)$")
_TOTALS_RE = re.compile(r"(\d+)\s+passed,\s*(\d+)\s+failed,\s*(\d+)\s+warnings?")

LEVELS = {"PASS": "pass", "FAIL": "fail", "WARN": "warn"}


def parse_verify_box(output: str, exit_code: Optional[int] = None) -> dict:
    """Parse verify_box.sh output into sections of checks.

    Lines it prints:
      `== Section ==`          starts a section ("RESULT" is the trailer)
      `  [PASS] text`          a check (also FAIL / WARN)
      `         more text`     detail under the previous check (e.g. the
                               list of failed systemd units)
    Anything else (blank lines, stray tool output) is ignored.
    """
    text = _ANSI_RE.sub("", output or "")
    sections = []
    current = None
    in_result = False
    totals = None
    verdict = None
    last_check = None
    for raw in text.splitlines():
        line = raw.rstrip()
        m = _SECTION_RE.match(line)
        if m:
            name = m.group(1)
            in_result = name.upper() == "RESULT"
            last_check = None
            if not in_result:
                current = {"name": name, "checks": []}
                sections.append(current)
            continue
        if in_result:
            t = _TOTALS_RE.search(line)
            if t:
                totals = tuple(int(x) for x in t.groups())
            elif "NOT SHIPPABLE" in line:
                verdict = False
            elif "SHIPPABLE" in line:
                verdict = True
            continue
        m = _CHECK_RE.match(line)
        if m:
            if current is None:
                current = {"name": "General", "checks": []}
                sections.append(current)
            last_check = {"level": LEVELS[m.group(1)], "text": m.group(2).strip()}
            current["checks"].append(last_check)
            continue
        if last_check is not None and line.startswith("   ") and line.strip():
            last_check.setdefault("details", []).append(line.strip())

    counts = {lvl: sum(1 for s in sections for c in s["checks"] if c["level"] == lvl)
              for lvl in ("pass", "fail", "warn")}
    result = {
        "sections": sections,
        "passed": counts["pass"],
        "failed": counts["fail"],
        "warnings": counts["warn"],
        "exit_code": exit_code,
        "shippable": (verdict if verdict is not None
                      else (exit_code == 0 and counts["fail"] == 0
                            if exit_code is not None else None)),
    }
    if totals is not None:
        result["script_totals"] = {"passed": totals[0], "failed": totals[1],
                                   "warnings": totals[2]}
    return result


def headline(result: dict) -> str:
    """The one plain-language line the card shows."""
    if not result or result.get("error"):
        return "The health check could not run"
    failed, warns = result.get("failed", 0), result.get("warnings", 0)
    if failed:
        return f"{failed} problem{'s' if failed != 1 else ''} found"
    if result.get("exit_code") not in (0, None):
        return "The health check did not finish cleanly"
    if warns:
        return f"Everything important looks good ({warns} note{'s' if warns != 1 else ''})"
    return "Everything looks good"


def _atomic_write_json(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class HealthRunner:
    def __init__(self, script: Path, cache_path: Path,
                 run: Callable = subprocess.run,
                 redactor_factory: Callable[[], Redactor] = Redactor,
                 clock: Callable[[], float] = time.time):
        self.script = Path(script)
        self.cache_path = Path(cache_path)
        self._run = run
        self._redactor_factory = redactor_factory
        self._clock = clock
        self._lock = threading.Lock()
        self._running = False
        self._started_at: Optional[float] = None
        self._with_services = False
        self._thread: Optional[threading.Thread] = None

    def argv(self, with_services: bool) -> list:
        # Fixed argv. bash explicitly: a tarball that lost the exec bit
        # (network_doctor.sh shipped 0644 once) must not break the card.
        cmd = ["sudo", "-n", "/bin/bash", str(self.script)]
        if with_services:
            cmd.append("--with-services")
        return cmd

    def start(self, with_services: bool = False) -> bool:
        """Start a run in the background. False if one is already running."""
        with self._lock:
            if self._running:
                return False
            self._running = True
            self._started_at = self._clock()
            self._with_services = bool(with_services)
        t = threading.Thread(target=self._worker, args=(bool(with_services),),
                             name="box-health", daemon=True)
        self._thread = t
        t.start()
        return True

    def join(self, timeout: Optional[float] = None) -> None:
        if self._thread is not None:
            self._thread.join(timeout)

    def _worker(self, with_services: bool) -> None:
        started = self._started_at or self._clock()
        try:
            result = self._execute(with_services)
        except Exception as e:  # never leave the card stuck on "running"
            result = {"sections": [], "passed": 0, "failed": 0, "warnings": 0,
                      "exit_code": None, "shippable": None,
                      "error": f"internal error: {type(e).__name__}"}
        result["with_services"] = with_services
        result["started_at"] = _iso(started)
        result["finished_at"] = _iso(self._clock())
        result["duration_s"] = round(self._clock() - started, 1)
        result["headline"] = headline(result)
        try:
            _atomic_write_json(self.cache_path, result)
        except OSError:
            pass
        finally:
            with self._lock:
                self._last_in_memory = result
                self._running = False

    def _execute(self, with_services: bool) -> dict:
        if not self.script.is_file():
            return {"sections": [], "passed": 0, "failed": 0, "warnings": 0,
                    "exit_code": None, "shippable": None,
                    "error": "verify_box.sh is not installed on this box"}
        timeout = RUN_TIMEOUT_WITH_SERVICES_S if with_services else RUN_TIMEOUT_S
        env = {k: v for k, v in os.environ.items() if k in ("PATH", "LANG", "LC_ALL")}
        env.setdefault("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
        try:
            proc = self._run(self.argv(with_services), capture_output=True, text=True,
                             timeout=timeout, stdin=subprocess.DEVNULL, env=env)
        except subprocess.TimeoutExpired as e:
            out = e.stdout if isinstance(e.stdout, str) else (e.stdout or b"").decode("utf-8", "replace")
            result = self._parse(out, None)
            result["error"] = f"the check did not finish within {timeout // 60} minutes"
            return result
        except OSError as e:
            return {"sections": [], "passed": 0, "failed": 0, "warnings": 0,
                    "exit_code": None, "shippable": None,
                    "error": f"could not start the check ({e.strerror or e})"}
        result = self._parse(proc.stdout or "", proc.returncode)
        if not result["sections"]:
            err = (proc.stderr or "").strip()
            if "password is required" in err or "a terminal is required" in err:
                result["error"] = "the web service is not allowed to run the check (sudo)"
            else:
                tail = self._redactor_factory().text(err[-300:], strict=True) if err else ""
                result["error"] = "the check produced no results" + (f": {tail}" if tail else "")
        return result

    def _parse(self, output: str, exit_code: Optional[int]) -> dict:
        result = parse_verify_box(output[:MAX_OUTPUT_CHARS], exit_code)
        # verify_box.sh prints no secrets by design; this is defence in
        # depth for the --with-services tail it quotes. Value-level only
        # (strict=False): "[PASS] qBit login with .env password succeeds"
        # is a useful line with nothing secret in it.
        r = self._redactor_factory()
        for s in result["sections"]:
            for c in s["checks"]:
                c["text"] = r.line(c["text"], strict=False)
                if "details" in c:
                    c["details"] = [r.line(d, strict=False) for d in c["details"][:50]]
        return result

    def status(self) -> dict:
        with self._lock:
            running = self._running
            started = self._started_at
            with_services = self._with_services
        last = self.last_result()
        return {
            "running": running,
            "started_at": _iso(started) if running and started else None,
            "running_with_services": with_services if running else None,
            "result": last,
        }

    def last_result(self) -> Optional[dict]:
        try:
            doc = json.loads(self.cache_path.read_text(encoding="utf-8"))
            return doc if isinstance(doc, dict) else None
        except (OSError, ValueError):
            return getattr(self, "_last_in_memory", None)


def _iso(ts: Optional[float]) -> Optional[str]:
    if ts is None:
        return None
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
