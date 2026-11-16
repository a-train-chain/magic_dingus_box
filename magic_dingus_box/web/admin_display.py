"""Tube CRT controls for the Content Manager: what the kiosk holds, and live
tuning from the page.

The kiosk publishes its Tube state as the "tube" block of kiosk_status.json
(app/status_writer.cpp) and reads data/tube_request.json, a log of numbered
operations (app/tube_ops.h). This module is that log's ONLY writer:

* GET  /admin/display/tube — always 200. Offered: the row file
  (static/tube_rows.json) and the kiosk's tube block; `?state_only=1` leaves
  the rows out (the page polls). Not offered: `available: false` and why —
  kiosk_not_running (status missing, unreadable or older than 10 s),
  kiosk_too_old (a kiosk without the feature publishes no "tube" key), the
  kiosk's own reason (needs_pi5 / not_ready), or rows_unavailable (the row
  file is missing or unreadable — a bad merge, a tree caught mid-update; the
  rest of the Content Manager keeps running). The page hides itself then.
* POST /admin/display/tube — 1..64 ops, validated all-or-nothing against the
  row file, then numbered from max(top seq in the file, the kiosk's
  handled_seq) + 1. Ops the kiosk has handled are dropped and only the newest
  op per row key is kept, so a dragged slider does not grow the file. The
  result must stay under the kiosk's 16 KiB read cap; when it would not, the
  kiosk is not keeping up and the POST is refused (409 TUBE_BACKLOG).

The kiosk is the only writer of settings.json: nothing here touches it. The
web admin is one process with many threads, so the read-append-replace runs
under one module lock.
"""
from __future__ import annotations

import json
import math
import os
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

from flask import request

try:
    from admin_common import error_response, success_response
except ImportError:
    from .admin_common import error_response, success_response

REQUEST_NAME = "tube_request.json"
STATUS_NAME = "kiosk_status.json"
STATUS_MAX_AGE_S = 10.0
MAX_OPS_PER_POST = 64
MAX_FILE_BYTES = 16 * 1024  # app::kTubeOpsMaxBytes: a longer file is refused unread

ROWS_PATH = Path(__file__).parent / "static" / "tube_rows.json"

# Serializes every read-append-replace of the op log (gthread: many threads).
_log_lock = threading.Lock()


def load_rows() -> dict:
    """The row file the page draws from and validation checks against."""
    return json.loads(ROWS_PATH.read_text("utf-8"))


def _load_rows_checked() -> Optional[dict]:
    """load_rows(), or None (logged once) when the file is missing, unreadable,
    not JSON, or not the shape validation reads. Never raises: register() runs
    inside create_app(), and an exception there takes down every route."""
    try:
        rows = load_rows()
        _op_rules(rows)  # every field validation reads, with the right types
        if not (_is_int(rows["row_min"]) and _is_int(rows["row_max"])):
            raise ValueError("row_min/row_max are not integers")
        return rows
    except (OSError, ValueError, LookupError, TypeError, AttributeError) as e:
        print(f"WARNING: Tube controls off: {ROWS_PATH.name} unusable: {e!r}", file=sys.stderr)
        return None


def _is_int(value) -> bool:
    # bool is a subclass of int; the kiosk refuses true/false (and 20.0).
    return type(value) is int


def kiosk_tube(data_dir: Path, now: Optional[float] = None) -> tuple:
    """(tube block, None) when the kiosk offers Tube, else (None, reason)."""
    tube, reason, _ts = kiosk_tube_status(data_dir, now)
    return tube, reason


def kiosk_tube_status(data_dir: Path, now: Optional[float] = None) -> tuple:
    """kiosk_tube plus the status's own `ts` (None when there is no fresh one).

    The page compares `ts` with a POST's `accepted_at` (both this box's clock)
    so it calls a change lost only on a status the kiosk wrote well after the
    change was accepted, never on a status that is merely late."""
    try:
        status = json.loads((Path(data_dir) / STATUS_NAME).read_text("utf-8"))
    except (OSError, ValueError):  # missing, unreadable, not JSON (incl. bad UTF-8)
        return None, "kiosk_not_running", None
    if not isinstance(status, dict):
        return None, "kiosk_not_running", None
    ts = status.get("ts")
    if not isinstance(ts, (int, float)) or isinstance(ts, bool):
        return None, "kiosk_not_running", None
    try:
        ts_s = float(ts)  # an int too large for a float raises OverflowError
    except OverflowError:
        return None, "kiosk_not_running", None
    if not math.isfinite(ts_s):
        return None, "kiosk_not_running", None
    # Both directions: the Pi has no RTC, so the clock can step BACK after a
    # kiosk dies, which would leave its last status "fresh" for the step's size.
    if abs((time.time() if now is None else now) - ts_s) > STATUS_MAX_AGE_S:
        return None, "kiosk_not_running", None
    tube = status.get("tube")
    if not isinstance(tube, dict):
        return None, "kiosk_too_old", None
    if tube.get("gate") is not True:
        reason = tube.get("reason")
        return None, (reason if isinstance(reason, str) and reason else "not_ready"), None
    return tube, None, ts


def _op_rules(rows: dict) -> dict:
    """What a valid op may say, from the row file."""
    return {
        "row_keys": frozenset(row["key"] for row in rows["rows"]),
        "row_min": rows["row_min"],
        "row_max": rows["row_max"],
        "words": {
            "mask": frozenset(choice["key"] for choice in rows["mask"]["choices"]),
            "look": frozenset(look["key"] for look in rows["looks"]),
            "engine": frozenset(engine["key"] for engine in rows["engines"]),
        },
    }


def _check_op(op, rules: dict) -> tuple:
    """(clean op, None) or (None, field). Every request-supplied value is
    type-checked before it is hashed, compared or looked up, so no shape of
    JSON can raise here. The clean op keeps only the fields its kind names."""
    if not isinstance(op, dict):
        return None, "op"
    name = op.get("op")
    if not isinstance(name, str):
        return None, "op"
    if name == "row":
        key, value = op.get("key"), op.get("value")
        if not isinstance(key, str) or key not in rules["row_keys"]:
            return None, "key"
        if not _is_int(value) or not rules["row_min"] <= value <= rules["row_max"]:
            return None, "value"
        return {"op": "row", "key": key, "value": value}, None
    if name in rules["words"]:
        word = op.get(name)
        if not isinstance(word, str) or word not in rules["words"][name]:
            return None, name
        return {"op": name, name: word}, None
    if name in ("reset", "degauss"):
        return {"op": name}, None
    return None, "op"


def validate_ops(body, rows: dict) -> tuple:
    """(clean ops, None) or (None, {"index": i, "field": f}).

    All or nothing. A malformed body (not an object, `ops` not a list of
    1..MAX_OPS_PER_POST) is reported as {"index": None, "field": "ops"}.
    Each clean op keeps only the fields its kind names; `seq` is the server's.
    """
    ops = body.get("ops") if isinstance(body, dict) else None
    if not isinstance(ops, list) or not 1 <= len(ops) <= MAX_OPS_PER_POST:
        return None, {"index": None, "field": "ops"}
    rules = _op_rules(rows)
    clean = []
    for i, op in enumerate(ops):
        checked, field = _check_op(op, rules)
        if checked is None:
            return None, {"index": i, "field": field}
        clean.append(checked)
    return clean, None


def append_ops(existing: list, new: list, handled_seq: int) -> tuple:
    """(ops, top): the log after numbering `new` onto `existing`. Pure."""
    top = max([handled_seq] + [op["seq"] for op in existing])
    ops = [dict(op) for op in existing if op["seq"] > handled_seq]
    for op in new:
        top += 1
        if op["op"] == "row":
            ops = [kept for kept in ops if not (kept["op"] == "row" and kept.get("key") == op["key"])]
        ops.append({"seq": top, **op})
    return ops, top


def _read_log(path: Path, rows: dict) -> list:
    """The ops already in the log. A missing file, or one the kiosk's parser
    would refuse (app/tube_ops.cpp: version 1, every op valid, seq integers
    >= 1 strictly ascending), holds none: keeping a refused file would
    re-write it forever while the kiosk's handled_seq never moves."""
    try:
        doc = json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        return []
    if not isinstance(doc, dict) or doc.get("version") != 1 or not _is_int(doc.get("version")):
        return []
    ops = doc.get("ops")
    if not isinstance(ops, list):
        return []
    rules = _op_rules(rows)
    kept = []
    previous = 0
    for op in ops:
        checked, _ = _check_op(op, rules)
        seq = op.get("seq") if isinstance(op, dict) else None
        if checked is None or not _is_int(seq) or seq <= previous:
            return []
        previous = seq
        kept.append({"seq": seq, **checked})
    return kept


def _serialize(ops: list) -> bytes:
    return json.dumps({"version": 1, "ops": ops}, separators=(",", ":")).encode("utf-8")


def _replace_atomically(data_dir: Path, text: bytes) -> bool:
    """Temp file in the same directory, then os.replace. No fsync: the log is
    transient (the kiosk persists what it applies to settings.json)."""
    tmp = None
    try:
        fd, tmp = tempfile.mkstemp(dir=data_dir, prefix=".tube_request.", suffix=".tmp")
        with os.fdopen(fd, "wb") as f:
            f.write(text)
            os.fchmod(f.fileno(), 0o644)  # mkstemp makes 0600; match the data dir
        os.replace(tmp, data_dir / REQUEST_NAME)
        return True
    except OSError:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        return False


def register(app, ctx) -> None:
    data_dir = Path(ctx.data_dir)
    require_csrf = ctx.require_csrf
    rows = _load_rows_checked()  # None: the routes answer rows_unavailable

    @app.get("/admin/display/tube")
    def tube_state():  # type: ignore[no-redef]
        if rows is None:
            return success_response({"available": False, "reason": "rows_unavailable"})
        tube, reason, ts = kiosk_tube_status(data_dir)
        if tube is None:
            return success_response({"available": False, "reason": reason})
        data = {"available": True, "rows": rows, "state": tube, "status_ts": ts}
        if request.args.get("state_only") == "1":
            del data["rows"]
        return success_response(data)

    @app.post("/admin/display/tube")
    @require_csrf
    def tube_ops():  # type: ignore[no-redef]
        if rows is None:
            return error_response("TUBE_UNAVAILABLE", "The TV is not offering the Tube engine",
                                  409, details={"reason": "rows_unavailable"})
        ops, problem = validate_ops(request.get_json(silent=True), rows)
        if ops is None:
            return error_response("VALIDATION_ERROR", "Invalid Tube operation", 400, details=problem)

        tube, reason = kiosk_tube(data_dir)
        if tube is None:
            return error_response("TUBE_UNAVAILABLE", "The TV is not offering the Tube engine",
                                  409, details={"reason": reason})
        handled = tube.get("handled_seq")
        handled_seq = handled if _is_int(handled) and handled >= 0 else 0

        with _log_lock:
            existing = _read_log(data_dir / REQUEST_NAME, rows)
            log_ops, top = append_ops(existing, ops, handled_seq)
            text = _serialize(log_ops)
            if len(text) > MAX_FILE_BYTES:
                return error_response("TUBE_BACKLOG", "The TV is not keeping up; try again shortly", 409)
            if not _replace_atomically(data_dir, text):
                return error_response("INTERNAL_ERROR", "Could not save the change", 500)
        return success_response({"seq": top, "accepted_at": time.time()})
