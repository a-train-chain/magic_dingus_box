"""The Tube CRT routes (admin_display.py): what the kiosk holds, and the op log
(data/tube_request.json) the page's changes are appended to for the kiosk."""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import admin_display  # noqa: E402

ROWS = admin_display.load_rows()
ROW_KEYS = [r["key"] for r in ROWS["rows"]]

OPEN = {
    "gate": True,
    "engine": "tube",
    "effective_engine": "tube",
    "crt_native": False,
    "look": "pvm",
    "mask": "grille",
    "custom": False,
    "moved": [],
    "values": {key: 50 for key in ROW_KEYS},
    "handled_seq": 0,
    "degauss_requests": 0,
}
HUM = {"op": "row", "key": "hum", "value": 20}


def write_status(data_dir: Path, tube, ts=None) -> None:
    doc = {"schema": 1, "ts": time.time() if ts is None else ts, "tube": tube}
    (data_dir / "kiosk_status.json").write_text(json.dumps(doc))


def post(client, ops):
    return client.post("/admin/display/tube", json={"ops": ops})


def log(data_dir: Path) -> dict:
    return json.loads((data_dir / "tube_request.json").read_text())


def test_open_block_has_44_values():
    assert len(OPEN["values"]) == 44


def test_get_returns_rows_and_state(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is True
    assert len(data["rows"]["rows"]) == 44 and data["state"] == OPEN
    data = client.get("/admin/display/tube?state_only=1").get_json()["data"]
    assert "rows" not in data and data["state"]["handled_seq"] == 0


def test_get_says_kiosk_not_running_when_status_is_stale(client, temp_data_dir):
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "kiosk_not_running"
    write_status(temp_data_dir, OPEN, ts=time.time() - 30)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is False and data["reason"] == "kiosk_not_running"


def test_get_says_kiosk_not_running_when_status_is_unparsable(client, temp_data_dir):
    (temp_data_dir / "kiosk_status.json").write_text("{not json")
    r = client.get("/admin/display/tube")
    assert r.status_code == 200 and r.get_json()["data"]["reason"] == "kiosk_not_running"


def test_get_says_kiosk_too_old_without_a_tube_block(client, temp_data_dir):
    (temp_data_dir / "kiosk_status.json").write_text(json.dumps({"schema": 1, "ts": time.time()}))
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "kiosk_too_old"
    assert post(client, [HUM]).status_code == 409


@pytest.mark.parametrize("reason", ["needs_pi5", "not_ready"])
def test_not_offered_is_reported_and_writes_nothing(client, temp_data_dir, reason):
    write_status(temp_data_dir, {"gate": False, "reason": reason, "engine": "enhanced",
                                 "effective_engine": "enhanced"})
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data == {"available": False, "reason": reason}
    r = post(client, [HUM])
    assert r.status_code == 409 and r.get_json()["error"]["details"]["reason"] == reason
    assert r.get_json()["error"]["code"] == "TUBE_UNAVAILABLE"
    assert not (temp_data_dir / "tube_request.json").exists()


def test_post_appends_ops_with_increasing_seq(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    assert post(client, [{"op": "look", "look": "tv"}, HUM]).get_json()["data"]["seq"] == 2
    assert post(client, [{"op": "degauss"}]).get_json()["data"]["seq"] == 3
    assert log(temp_data_dir) == {"version": 1, "ops": [
        {"seq": 1, "op": "look", "look": "tv"},
        {"seq": 2, "op": "row", "key": "hum", "value": 20},
        {"seq": 3, "op": "degauss"}]}


def test_post_keeps_only_the_named_fields(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    post(client, [{"op": "reset", "seq": 99, "extra": 1},
                  {"op": "engine", "engine": "classic", "key": "hum"},
                  {"op": "mask", "mask": "dots", "value": 3}])
    assert log(temp_data_dir)["ops"] == [
        {"seq": 1, "op": "reset"},
        {"seq": 2, "op": "engine", "engine": "classic"},
        {"seq": 3, "op": "mask", "mask": "dots"}]


def test_handled_ops_are_dropped_and_seq_continues(client, temp_data_dir):
    write_status(temp_data_dir, OPEN); post(client, [HUM, HUM])
    write_status(temp_data_dir, {**OPEN, "handled_seq": 2})
    post(client, [{"op": "reset"}])
    assert log(temp_data_dir)["ops"] == [{"seq": 3, "op": "reset"}]


def test_seq_continues_past_what_the_kiosk_handled(client, temp_data_dir):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 57})        # the file was wiped
    assert post(client, [HUM]).get_json()["data"]["seq"] == 58


def test_an_unparsable_log_counts_as_no_ops(client, temp_data_dir):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 4})
    (temp_data_dir / "tube_request.json").write_text("garbage")
    assert post(client, [HUM]).get_json()["data"]["seq"] == 5
    assert log(temp_data_dir)["ops"] == [{"seq": 5, "op": "row", "key": "hum", "value": 20}]


GOOD = {"seq": 7, "op": "row", "key": "noise", "value": 30}


@pytest.mark.parametrize("doc", [
    {"version": 1, "ops": [GOOD, {"seq": 8, "op": "wobble"}]},                       # unknown op
    {"version": 1, "ops": [GOOD, {"seq": 8, "op": "row", "key": "hum", "value": 101}]},  # bad value
    {"version": 1, "ops": [GOOD, {"seq": 8, "op": "row", "key": "hum", "value": 2.0}]},
    {"version": 1, "ops": [GOOD, {"seq": 8, "op": "mask", "mask": "Slot"}]},
    {"version": 1, "ops": [GOOD, {"seq": 7, "op": "degauss"}]},                      # not ascending
    {"version": 1, "ops": [{**GOOD, "seq": 9}, {"seq": 8, "op": "degauss"}]},
    {"version": 1, "ops": [{**GOOD, "seq": 0}]},                                      # seq < 1
    {"version": 1, "ops": [{**GOOD, "seq": -3}]},
    {"version": 1, "ops": [{**GOOD, "seq": True}]},
    {"version": 1, "ops": [{**GOOD, "seq": 7.0}]},
    {"version": 1, "ops": [{"op": "degauss"}]},                                       # no seq
    {"version": 1, "ops": [[], GOOD]},
    {"version": 2, "ops": [GOOD]},
    {"version": True, "ops": [GOOD]},
    {"ops": [GOOD]},
    {"version": 1, "ops": {"0": GOOD}},
    [GOOD],
])
def test_a_log_the_kiosk_would_refuse_is_replaced_not_kept(client, temp_data_dir, doc):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 4})
    (temp_data_dir / "tube_request.json").write_text(json.dumps(doc))
    assert post(client, [HUM]).get_json()["data"]["seq"] == 5
    assert log(temp_data_dir)["ops"] == [{"seq": 5, "op": "row", "key": "hum", "value": 20}]


def test_a_valid_log_is_kept_and_cleaned(client, temp_data_dir):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 4})
    (temp_data_dir / "tube_request.json").write_text(json.dumps(
        {"version": 1, "ops": [{"seq": 3, "op": "degauss"}, {**GOOD, "extra": 1}]}))
    assert post(client, [HUM]).get_json()["data"]["seq"] == 8
    assert log(temp_data_dir)["ops"] == [GOOD, {"seq": 8, "op": "row", "key": "hum", "value": 20}]


def test_the_log_is_world_readable(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    post(client, [HUM])
    assert (temp_data_dir / "tube_request.json").stat().st_mode & 0o777 == 0o644


def test_rapid_posts_keep_only_the_newest_op_per_row(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    seqs = [post(client, [{**HUM, "value": v}]).get_json()["data"]["seq"] for v in range(0, 101, 5)]
    assert seqs == list(range(1, 22))
    assert log(temp_data_dir)["ops"] == [{"seq": 21, "op": "row", "key": "hum", "value": 100}]
    assert not list(temp_data_dir.glob(".tube_request.*"))


def test_two_pages_ops_both_land_in_seq_order(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    post(client, [HUM])                                              # page A
    post(client, [{"op": "row", "key": "noise", "value": 30}])      # page B
    post(client, [{**HUM, "value": 25}])                             # page A again
    assert log(temp_data_dir)["ops"] == [
        {"seq": 2, "op": "row", "key": "noise", "value": 30},
        {"seq": 3, "op": "row", "key": "hum", "value": 25}]


@pytest.mark.parametrize("op,field", [({"op": "wobble"}, "op"), ({"op": "row", "key": "wobble", "value": 5}, "key"),
    ({"op": "row", "key": "hum", "value": 101}, "value"), ({"op": "row", "key": "hum", "value": "20"}, "value"),
    ({"op": "row", "key": "hum", "value": True}, "value"), ({"op": "row", "key": "hum", "value": 20.0}, "value"),
    ({"op": "mask", "mask": "Slot"}, "mask"), ({"op": "look", "look": "vhs"}, "look"),
    ({"op": "engine", "engine": "plasma"}, "engine")])
def test_post_refuses_a_bad_op_and_writes_nothing(client, temp_data_dir, op, field):
    write_status(temp_data_dir, OPEN)
    r = post(client, [HUM, op])
    assert r.status_code == 400 and r.get_json()["error"]["details"] == {"index": 1, "field": field}
    assert not (temp_data_dir / "tube_request.json").exists()


@pytest.mark.parametrize("op,field", [("row", "op"), ({"op": 3}, "op"), ({"op": "row", "value": 5}, "key"),
    ({"op": "row", "key": "hum"}, "value"), ({"op": "row", "key": "hum", "value": -1}, "value"),
    ({"op": "mask"}, "mask"), ({"op": "look", "look": 1}, "look"), ({"op": "engine", "engine": None}, "engine"),
    ({"op": []}, "op"), ({"op": {}}, "op")])
def test_post_refuses_more_bad_ops(client, temp_data_dir, op, field):
    write_status(temp_data_dir, OPEN)
    r = post(client, [op])
    assert r.status_code == 400 and r.get_json()["error"]["details"] == {"index": 0, "field": field}
    assert r.get_json()["error"]["code"] == "VALIDATION_ERROR"


WRONG_TYPES = [[], {}, None, 3, 2.5, True, "", {"a": {"b": [1, {"c": None}]}}, [[{"x": []}]]]


def _wrong_typed_bodies():
    """(body, expected details) for every request-supplied field, each wrong-typed."""
    for bad in WRONG_TYPES:
        if not (isinstance(bad, list) and bad):  # a non-empty list is a valid container
            yield {"ops": bad}, {"index": None, "field": "ops"}
        yield {"ops": [bad]}, {"index": 0, "field": "op"}
        yield {"ops": [{"op": bad}]}, {"index": 0, "field": "op"}
        yield {"ops": [{"op": "row", "key": bad, "value": 5}]}, {"index": 0, "field": "key"}
        if type(bad) is not int:  # 3 is a valid row value
            yield {"ops": [{"op": "row", "key": "hum", "value": bad}]}, {"index": 0, "field": "value"}
        for word in ("mask", "look", "engine"):
            yield {"ops": [{"op": word, word: bad}]}, {"index": 0, "field": word}
    for bad in WRONG_TYPES:
        if not isinstance(bad, dict):
            yield bad, {"index": None, "field": "ops"}


@pytest.mark.parametrize("body,details", list(_wrong_typed_bodies()))
def test_every_wrong_typed_field_is_a_400_envelope(client, temp_data_dir, body, details):
    write_status(temp_data_dir, OPEN)
    r = client.post("/admin/display/tube", json=body)
    assert r.status_code == 400 and r.is_json
    assert r.get_json()["error"] == {"code": "VALIDATION_ERROR", "message": r.get_json()["error"]["message"],
                                     "details": details}
    assert not (temp_data_dir / "tube_request.json").exists()


def test_post_refuses_an_empty_or_oversized_batch(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    assert post(client, []).status_code == 400
    assert post(client, [HUM] * (admin_display.MAX_OPS_PER_POST + 1)).status_code == 400
    assert client.post("/admin/display/tube", json=[HUM]).status_code == 400
    assert client.post("/admin/display/tube", json={"ops": HUM}).status_code == 400
    assert client.post("/admin/display/tube", data="nope", content_type="text/plain").status_code == 400
    assert not (temp_data_dir / "tube_request.json").exists()


def test_a_full_batch_of_every_row_fits(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    ops = [{"op": "row", "key": key, "value": 100} for key in ROW_KEYS]
    ops += [{"op": "look", "look": "everything"}] * (admin_display.MAX_OPS_PER_POST - len(ops))
    assert post(client, ops).get_json()["data"]["seq"] == 64
    assert len(log(temp_data_dir)["ops"]) == 64


def test_the_log_never_passes_the_kiosk_size_cap(client, temp_data_dir):
    """The kiosk stops reading (handled_seq stays 0) while a client keeps posting
    full batches: the file grows to the cap, then posts are refused, unwritten."""
    write_status(temp_data_dir, OPEN)
    batch = [{"op": "engine", "engine": "enhanced"}] * admin_display.MAX_OPS_PER_POST
    statuses = [post(client, batch).status_code for _ in range(20)]
    assert statuses[0] == 200 and statuses[-1] == 409
    assert statuses == sorted(statuses)                  # once refused, refused
    r = post(client, batch)
    assert r.get_json()["error"]["code"] == "TUBE_BACKLOG"
    path = temp_data_dir / "tube_request.json"
    size = path.stat().st_size
    assert size <= admin_display.MAX_FILE_BYTES
    doc = json.loads(path.read_text())
    seqs = [op["seq"] for op in doc["ops"]]
    assert seqs == list(range(1, len(seqs) + 1))
    assert not list(temp_data_dir.glob(".tube_request.*"))
    # Once the kiosk catches up, posting works again.
    write_status(temp_data_dir, {**OPEN, "handled_seq": seqs[-1]})
    assert post(client, batch).get_json()["data"]["seq"] == seqs[-1] + 64


def test_a_failed_write_leaves_no_temp_file(client, temp_data_dir, monkeypatch):
    write_status(temp_data_dir, OPEN)

    def broken_replace(src, dst):
        raise OSError("disk full")

    monkeypatch.setattr(admin_display.os, "replace", broken_replace)
    r = post(client, [HUM])
    assert r.status_code == 500 and r.get_json()["error"]["code"] == "INTERNAL_ERROR"
    assert not (temp_data_dir / "tube_request.json").exists()
    assert not list(temp_data_dir.glob(".tube_request.*"))


def test_post_requires_csrf(client, temp_data_dir, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF")
    write_status(temp_data_dir, OPEN)
    r = client.post("/admin/display/tube", json={"ops": [HUM]})
    assert r.status_code == 403 and r.get_json()["error"]["code"] == "CSRF_ERROR"


def test_never_touches_settings(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    before = {p.name for p in temp_data_dir.rglob("*")}
    post(client, [{"op": "engine", "engine": "classic"}, HUM])
    assert {p.name for p in temp_data_dir.rglob("*")} - before == {"tube_request.json"}
    assert not list(temp_data_dir.parent.rglob("settings.json"))


# --- the pure helpers ----------------------------------------------------------

def test_kiosk_tube_reads_age_from_ts(temp_data_dir):
    write_status(temp_data_dir, OPEN, ts=1000.0)
    assert admin_display.kiosk_tube(temp_data_dir, now=1005.0) == (OPEN, None)
    assert admin_display.kiosk_tube(temp_data_dir, now=1011.0) == (None, "kiosk_not_running")
    (temp_data_dir / "kiosk_status.json").write_text(json.dumps({"ts": True, "tube": OPEN}))
    assert admin_display.kiosk_tube(temp_data_dir) == (None, "kiosk_not_running")


def test_kiosk_tube_counts_a_future_ts_as_stale_too(temp_data_dir):
    """No RTC: the clock can step back after the kiosk dies."""
    write_status(temp_data_dir, OPEN, ts=1000.0)
    assert admin_display.kiosk_tube(temp_data_dir, now=995.0) == (OPEN, None)
    assert admin_display.kiosk_tube(temp_data_dir, now=989.0) == (None, "kiosk_not_running")
    write_status(temp_data_dir, OPEN, ts=time.time() + 30)
    assert admin_display.kiosk_tube(temp_data_dir) == (None, "kiosk_not_running")


@pytest.mark.parametrize("ts", ["NaN", "Infinity", "-Infinity", '"1000"', "null"])
def test_kiosk_tube_refuses_a_non_finite_ts(temp_data_dir, ts):
    (temp_data_dir / "kiosk_status.json").write_text('{"ts": %s, "tube": {"gate": true}}' % ts)
    assert admin_display.kiosk_tube(temp_data_dir, now=1000.0) == (None, "kiosk_not_running")


def test_append_ops_is_pure():
    existing = [{"seq": 3, "op": "row", "key": "hum", "value": 1},
                {"seq": 4, "op": "row", "key": "noise", "value": 2}]
    new = [{"op": "row", "key": "noise", "value": 9}, {"op": "degauss"}]
    ops, top = admin_display.append_ops(existing, new, handled_seq=3)
    assert top == 6 and ops == [{"seq": 5, "op": "row", "key": "noise", "value": 9},
                                {"seq": 6, "op": "degauss"}]
    assert existing[1] == {"seq": 4, "op": "row", "key": "noise", "value": 2}
    assert new[0] == {"op": "row", "key": "noise", "value": 9}


def test_the_written_log_is_what_the_kiosk_parser_accepts(client, temp_data_dir):
    """Integers only, strictly ascending seq, version 1, no other top-level keys."""
    write_status(temp_data_dir, OPEN)
    post(client, [{"op": "look", "look": "tv"}, HUM, {"op": "mask", "mask": "none"},
                  {"op": "reset"}, {"op": "engine", "engine": "tube"}, {"op": "degauss"}])
    doc = log(temp_data_dir)
    assert set(doc) == {"version", "ops"} and type(doc["version"]) is int
    seqs = [op["seq"] for op in doc["ops"]]
    assert all(type(s) is int for s in seqs) and seqs == sorted(set(seqs))
    assert os.path.getsize(temp_data_dir / "tube_request.json") <= admin_display.MAX_FILE_BYTES


def test_get_and_post_carry_the_times_the_page_compares(client, temp_data_dir):
    """The page calls a change lost only when a status the kiosk wrote well
    after the server accepted it still lacks its seq: GET gives the status's
    own ts, POST the acceptance time, both on this box's clock."""
    write_status(temp_data_dir, OPEN, ts=time.time() - 2.5)
    before = time.time()
    data = client.get("/admin/display/tube?state_only=1").get_json()["data"]
    assert data["status_ts"] == pytest.approx(before - 2.5, abs=0.5)
    accepted = post(client, [HUM]).get_json()["data"]
    assert accepted["seq"] == 1
    assert before <= accepted["accepted_at"] <= time.time()
    write_status(temp_data_dir, {"gate": False, "reason": "not_ready"})
    assert "status_ts" not in client.get("/admin/display/tube").get_json()["data"]
