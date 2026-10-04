#!/usr/bin/env python3
"""Tree snapshots and comparisons for the OTA rehearsal.

  manifest.py snap <root> <out.json>
      sha256 + mode + uid/gid of every file/symlink/dir under <root>.
  manifest.py user <before.json> <after.json>
      OPERATOR DATA must be byte-identical (content, mode, owner). The set is
      the "What's PRESERVED" table of OTA_UPDATE_GUARANTEES.md, written out
      here independently of update.sh's rsync excludes — so the check can
      catch an exclude list that drifts from the contract.
  manifest.py same <before.json> <after.json> [allow-glob ...]
      whole-tree equality (rollback): every difference must match an
      allow-glob; prints each difference with its classification.
  manifest.py release <release_root> <install_root>
      the installed tree matches the release tree: every release file is
      installed with identical bytes and exec bit (default playlists aside
      — the box keeps its own), and every installed file the release lacks
      is operator/runtime data, not a stale leftover the install's --delete
      should have removed.
Exit 0 = OK, 1 = a violation (printed as "VIOLATION ...").
"""
import fnmatch
import hashlib
import json
import os
import stat
import sys

D = "magic_dingus_box_cpp/data/"
USER_DATA = [
    D + "media/*", D + "roms/*", D + "saves/*", D + "states/*", D + "playlists/*",
    D + "thumbnails/*",            # per-game cover art; systems/ is carved out below
    D + "device_info.json", D + "paired_remotes.json", D + "flask_secret.key",
    D + "pairing_session.json", D + "pairing_audit.log", D + "media_browser.db*",
    "config/*", "services/.env", "services/config/*",
]
NOT_USER_DATA = [D + "thumbnails/systems/*",   # shipped, updated by OTA
                 "config/magic_dingus_box.log*"]  # the kiosk's own log ($CONFIG), runtime

# Present on a box, legitimately absent from a release tarball.
RUNTIME = [
    "magic_dingus_box_cpp/build/*", "tmp/*", "*/__pycache__/*", "*/__pycache__",
    D + "kiosk_status.json", D + "text_input_queue.jsonl", D + "seek_request.json",
    D + "upload_temp/*", D + "pending_revocations.txt", D + "screenshots/*",
    "config/magic_dingus_box.log*",
]


def match(path, globs):
    return any(fnmatch.fnmatchcase(path, g) or fnmatch.fnmatchcase(path + "/", g) for g in globs)


def is_user(path):
    return match(path, USER_DATA) and not match(path, NOT_USER_DATA)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def snap(root):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            st = os.lstat(p)
            e = {"mode": oct(stat.S_IMODE(st.st_mode)), "uid": st.st_uid, "gid": st.st_gid}
            if stat.S_ISLNK(st.st_mode):
                e.update(type="link", target=os.readlink(p))
            elif stat.S_ISDIR(st.st_mode):
                e.update(type="dir")
            else:
                e.update(type="file", size=st.st_size, sha=sha256(p))
            out[rel] = e
    return out


def load(p):
    with open(p) as f:
        return json.load(f)


def cmd_user(before, after):
    b, a = load(before), load(after)
    bad = 0
    n = 0
    for path, e in sorted(b.items()):
        if not is_user(path) or e["type"] == "dir":
            continue
        n += 1
        if path not in a:
            print(f"VIOLATION user data DELETED: {path}"); bad += 1
        elif a[path] != e:
            changed = [k for k in e if a[path].get(k) != e[k]]
            print(f"VIOLATION user data CHANGED ({','.join(changed)}): {path}"); bad += 1
    added = [p for p, e in a.items() if is_user(p) and p not in b and e["type"] != "dir"]
    for p in sorted(added):
        print(f"note: user-data area gained: {p}")
    print(f"user data: {n} files checked, {bad} violations, {len(added)} added")
    return 1 if bad else 0


def cmd_same(before, after, allow):
    b, a = load(before), load(after)
    bad = 0
    for path in sorted(set(b) | set(a)):
        eb, ea = b.get(path), a.get(path)
        if eb == ea:
            continue
        if eb and ea and eb.get("type") == "dir" == ea.get("type"):
            if eb["mode"] == ea["mode"] and eb["uid"] == ea["uid"]:
                continue   # dir mtimes are not tracked; mode/owner equal
        what = "added" if eb is None else "removed" if ea is None else \
            "changed(" + ",".join(k for k in eb if ea.get(k) != eb[k]) + ")"
        if match(path, allow):
            print(f"allowed {what}: {path}")
        else:
            print(f"VIOLATION {what}: {path}"); bad += 1
    print(f"tree compare: {bad} violations")
    return 1 if bad else 0


def cmd_release(rel_root, inst_root):
    bad = 0
    rel, inst = snap(rel_root), snap(inst_root)
    checked = 0
    for path, e in sorted(rel.items()):
        if e["type"] == "dir":
            continue
        if path.startswith(D + "playlists/"):
            continue   # the box keeps its own; add-only sync checked separately
        checked += 1
        i = inst.get(path)
        if i is None:
            print(f"VIOLATION release file NOT installed: {path}"); bad += 1
        elif e["type"] == "link":
            if i.get("target") != e.get("target"):
                print(f"VIOLATION symlink differs: {path}"); bad += 1
        elif i.get("sha") != e.get("sha"):
            print(f"VIOLATION installed bytes differ from release: {path}"); bad += 1
        elif (int(i["mode"], 8) & 0o111 != 0) != (int(e["mode"], 8) & 0o111 != 0):
            print(f"VIOLATION exec bit differs ({e['mode']} -> {i['mode']}): {path}"); bad += 1
    extra_user = extra_runtime = 0
    for path, i in sorted(inst.items()):
        if path in rel or i["type"] == "dir":
            continue
        if is_user(path):
            extra_user += 1
        elif match(path, RUNTIME):
            extra_runtime += 1
        else:
            print(f"VIOLATION installed file not in release (stale leftover?): {path}"); bad += 1
    print(f"release compare: {checked} release files checked; box-only files: "
          f"{extra_user} operator data, {extra_runtime} runtime/build; {bad} violations")
    return 1 if bad else 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "snap":
        with open(argv[2], "w") as f:
            json.dump(snap(argv[1]), f, indent=0, sort_keys=True)
        return 0
    if len(argv) == 3 and argv[0] == "user":
        return cmd_user(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "same":
        return cmd_same(argv[1], argv[2], argv[3:])
    if len(argv) == 3 and argv[0] == "release":
        return cmd_release(argv[1], argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
