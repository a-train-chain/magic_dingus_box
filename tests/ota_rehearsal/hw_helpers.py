#!/usr/bin/env python3
"""Pure helpers for tests/ota_rehearsal/hw_rehearsal.sh (hardware OTA rehearsal).

Runs in two places with the same code: on the Mac (comparisons, parsing,
tarball checks, the deploy --delete guard) and ON THE BOX, fed over ssh as
`python3 - <subcommand> ...` (manifests, fingerprints, listings) — so the
box needs nothing but python3, and filenames with spaces/newlines are never
round-tripped through a shell word list.

  manifest <install_root> <out.json>
      md5 of every APP-DATA file: <root>/config + <root>/magic_dingus_box_cpp/data,
      minus APPDATA_EXCLUDE (bulk content, live/runtime files, release-shipped data).
  fingerprint <out.json> <dir> [<dir> ...]
      {dir: {"files": N, "bytes": B}} (null for a missing dir) — the cheap
      content check for media/roms/saves/... and the movie drive.
  syslisting <out.json> [--root R]
      md5 / symlink target of the box's system state: installed units
      (SYSTEM_UNIT_GLOBS), every *.d drop-in and *.wants dir under
      /etc/systemd/system, /usr/local/bin, /etc/udev/rules.d, /etc/hosts.
  compare <before.json> <after.json> [--allow GLOB ...] [--label TEXT]
      every added / removed / changed key; exit 1 if any is not allowed.
  check-tarball <release.tar.gz>
      exit 1 on AppleDouble "._*" entries or git-lfs pointer files.
  kiosk-idle <kiosk_status.json>
      exit 0 iff screen == "playlist" and retroarch is null.
  rsync-deletions [--dest-prefix P]
      stdin = `rsync -n --itemize-changes` output; prints each deleted path
      (prefixed with P) one per line.
  deletion-violations [--dest-prefix P]
      same input; prints only deletions under data/ or config/; exit 1 if any.
  deploy-rsyncs <deploy_cpp.sh> [--var NAME=VALUE ...]
      every rsync in deploy_cpp.sh that passes --delete, parsed with shlex
      and with ${VARS} substituted; one JSON object per line
      {"step", "options", "filters", "src", "dest"}.
  deploy-guard <deploy_cpp.sh> --var ... [--print-only] [--log FILE]
      runs each of those rsyncs as `rsync -n --itemize-changes` (the exact
      flags and filter list deploy_cpp.sh uses) and exits 1 if any would
      delete something under data/ or config/.
  changed-keys <a.json> <b.json>
      keys that differ between two listings, one per line.
  get <file.json> <key> [<key> ...]
      a nested value ('' when absent) — JSON parsing for the bash driver.
  field-plan --file F [--old-units A --new-units B --changed-units C
                       --old-dropins D --new-dropins E]
      merges the per-release field-state file with the git-derived lists
      into the ordered strip plan, one "<directive> <arg>" per line.
"""
import fnmatch
import hashlib
import json
import os
import shlex
import stat
import subprocess
import sys
import tarfile

# ---------------------------------------------------------------------------
# App-data manifest scope. Paths are relative to the install root.
APPDATA_ROOTS = ["config", "magic_dingus_box_cpp/data"]
D = "magic_dingus_box_cpp/data/"
APPDATA_EXCLUDE = [
    # bulk content: covered by the counts+bytes fingerprint instead
    D + "media/*", D + "roms/*",
    # live / runtime files the kiosk and web admin rewrite on their own
    D + "screenshots/*", D + "upload_temp/*", "*.log", "*.log.*",
    D + "kiosk_status.json", D + "seek_request.json", D + "box_health_last.json",
    D + "media_browser.db", D + "media_browser.db-*", D + "text_input_queue.jsonl",
    "*/__pycache__/*",
    # release-shipped data (OTA_UPDATE_GUARANTEES.md "UPDATED" table): an
    # install / rollback legitimately swaps these with the release
    D + "intro/*", D + "thumbnails/systems/*",
]

SYSTEM_UNIT_GLOBS = ["magic-*", "gluetun-*", "qbit-*", "kiosk-*", "usb-gadget-*",
                     "led-*", "power-switch-*", "wifi-powersave-*", "content-manager-*"]
SYSTEMD_DIR = "/etc/systemd/system"
SYS_TREES = ["/usr/local/bin", "/etc/udev/rules.d"]
SYS_FILES = ["/etc/hosts"]

LFS_MAGIC = b"version https://git-lfs.github.com/spec/v1"
FIELD_DIRECTIVES = ("unit", "dropin", "old-unit", "file", "package", "pulse-client-conf", "run")
FIELD_ORDER = {d: i for i, d in enumerate(
    ("old-unit", "unit", "dropin", "file", "package", "pulse-client-conf", "run"))}


def die(msg, code=2):
    print(f"hw_helpers: {msg}", file=sys.stderr)
    sys.exit(code)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def entry(path):
    """md5 for a file, 'symlink:<target>' for a link, None for anything else."""
    st = os.lstat(path)
    if stat.S_ISLNK(st.st_mode):
        return "symlink:" + os.readlink(path)
    if stat.S_ISREG(st.st_mode):
        try:
            return md5(path)
        except OSError as e:
            return f"unreadable:{e.errno}"
    return None  # sockets, fifos, devices: never compared


def excluded(rel, globs):
    return any(fnmatch.fnmatchcase(rel, g) for g in globs)


def walk_files(top):
    """Every file / symlink under top (no following dir symlinks)."""
    for dirpath, dirnames, filenames in os.walk(top):
        for name in filenames + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]:
            yield os.path.join(dirpath, name)


def write_json(out, obj):
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    os.replace(tmp, out)


# ---------------------------------------------------------------------------
def cmd_manifest(args):
    root, out = args[0], args[1]
    result = {}
    for sub in APPDATA_ROOTS:
        top = os.path.join(root, sub)
        if not os.path.isdir(top):
            continue
        for p in walk_files(top):
            rel = os.path.relpath(p, root)
            if excluded(rel, APPDATA_EXCLUDE):
                continue
            e = entry(p)
            if e is not None:
                result[rel] = e
    write_json(out, result)
    print(f"manifest: {len(result)} app-data files -> {out}")


def cmd_fingerprint(args):
    out, dirs = args[0], args[1:]
    result = {}
    for d in dirs:
        if not os.path.isdir(d):
            result[d] = None
            continue
        n = b = 0
        for p in walk_files(d):
            st = os.lstat(p)
            if stat.S_ISREG(st.st_mode):
                n += 1
                b += st.st_size
        result[d] = {"files": n, "bytes": b}
    write_json(out, result)
    for d, v in result.items():
        print(f"fingerprint: {d}: {'absent' if v is None else '%d files, %d bytes' % (v['files'], v['bytes'])}")


def cmd_syslisting(args):
    out = args[0]
    root = ""
    if "--root" in args:  # test seam: a fake filesystem root
        root = args[args.index("--root") + 1].rstrip("/")
    result = {}

    def add(abs_path):
        p = root + abs_path
        if os.path.lexists(p):
            e = entry(p)
            if e is not None:
                result[abs_path] = e

    sd = root + SYSTEMD_DIR
    if os.path.isdir(sd):
        for name in sorted(os.listdir(sd)):
            p = os.path.join(sd, name)
            if os.path.isdir(p) and not os.path.islink(p) and (name.endswith(".d") or name.endswith(".wants")):
                for f in walk_files(p):
                    add(f[len(root):])
            elif any(fnmatch.fnmatchcase(name, g) for g in SYSTEM_UNIT_GLOBS):
                add(SYSTEMD_DIR + "/" + name)
    for t in SYS_TREES:
        if os.path.isdir(root + t):
            for f in walk_files(root + t):
                add(f[len(root):])
    for f in SYS_FILES:
        add(f)
    write_json(out, result)
    print(f"syslisting: {len(result)} entries -> {out}")


def compare(before, after, allow):
    """-> (violations, allowed) as lists of human-readable lines."""
    bad, ok = [], []
    for k in sorted(set(before) | set(after)):
        a, b = before.get(k, "<absent>"), after.get(k, "<absent>")
        if a == b:
            continue
        if k not in after:
            line = f"removed: {k}"
        elif k not in before:
            line = f"added:   {k}"
        else:
            line = f"changed: {k}"
        (ok if excluded(k, allow) else bad).append(line)
    return bad, ok


def cmd_compare(args):
    allow, label, files = [], "", []
    i = 0
    while i < len(args):
        if args[i] == "--allow":
            allow.append(args[i + 1]); i += 2
        elif args[i] == "--label":
            label = args[i + 1]; i += 2
        else:
            files.append(args[i]); i += 1
    if len(files) != 2:
        die("compare <before.json> <after.json> [--allow GLOB ...] [--label TEXT]")
    with open(files[0]) as f:
        before = json.load(f)
    with open(files[1]) as f:
        after = json.load(f)
    bad, ok = compare(before, after, allow)
    tag = f"[{label}] " if label else ""
    for line in ok:
        print(f"{tag}allowed {line}")
    for line in bad:
        print(f"{tag}DIFF    {line}")
    print(f"{tag}{len(before)} vs {len(after)} entries: {len(bad)} unexpected difference(s), {len(ok)} allowed")
    return 1 if bad else 0


def cmd_changed_keys(args):
    """Keys whose value differs (added, removed or changed), one per line."""
    with open(args[0]) as f:
        a = json.load(f)
    with open(args[1]) as f:
        b = json.load(f)
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            print(k)
    return 0


def cmd_get(args):
    """get <file.json|-> <key> [<key> ...] -> the nested value ('' if absent)."""
    try:
        if args[0] == "-":
            v = json.loads(sys.stdin.read())
        else:
            with open(args[0]) as f:
                v = json.load(f)
    except (OSError, ValueError):
        print("")
        return 1
    for k in args[1:]:
        v = v.get(k) if isinstance(v, dict) else None
    print("" if v is None else (json.dumps(v) if isinstance(v, (dict, list)) else v))
    return 0


def cmd_check_tarball(args):
    path = args[0]
    problems = []
    with tarfile.open(path, "r:*") as t:
        for m in t:
            base = os.path.basename(m.name.rstrip("/"))
            if base.startswith("._"):
                problems.append(f"AppleDouble entry: {m.name}")
            if m.isfile() and m.size < 1024:
                f = t.extractfile(m)
                if f is not None and f.read(len(LFS_MAGIC)) == LFS_MAGIC:
                    problems.append(f"git-lfs pointer, not content: {m.name}")
    for p in problems:
        print(p)
    print(f"check-tarball: {os.path.basename(path)}: {len(problems)} problem(s)")
    return 1 if problems else 0


def cmd_kiosk_idle(args):
    try:
        with open(args[0]) as f:
            s = json.load(f)
    except (OSError, ValueError) as e:
        print(f"kiosk status unreadable: {e}")
        return 1
    screen, ra = s.get("screen"), s.get("retroarch", "<missing>")
    if screen == "playlist" and ra is None:
        print("kiosk idle (screen=playlist, retroarch=null)")
        return 0
    print(f"kiosk NOT idle: screen={screen!r} retroarch={'null' if ra is None else json.dumps(ra)}")
    return 1


def parse_deletions(text, prefix=""):
    """rsync deletion lines -> [prefix+path]. Accepts the itemized form
    ('*deleting   <path>', GNU rsync pads, openrsync does not) and the plain
    -v form ('deleting <path>'); a directory may carry a trailing slash."""
    out = []
    for line in text.splitlines():
        for tag in ("*deleting ", "deleting "):
            if line.startswith(tag):
                p = line[len(tag):].lstrip(" ")
                if p:
                    out.append(prefix + p)
                break
    return out


PROTECTED_PREFIXES = ("data/", "config/", "magic_dingus_box_cpp/data/")


def is_protected(rel, prefix=""):
    """A deletion that would touch operator data or settings: anything under
    data/ or config/ relative to the rsync destination, or under the install
    tree's magic_dingus_box_cpp/data/ or config/ once the destination is
    accounted for."""
    for p in (rel.lstrip("/"), (prefix + rel).lstrip("/")):
        if p.rstrip("/") in ("data", "config", "magic_dingus_box_cpp/data") or p.startswith(PROTECTED_PREFIXES):
            return True
    return False


def _prefix(args):
    return args[args.index("--dest-prefix") + 1] if "--dest-prefix" in args else ""


def cmd_rsync_deletions(args):
    for p in parse_deletions(sys.stdin.read(), _prefix(args)):
        print(p)
    return 0


def cmd_deletion_violations(args):
    pre = _prefix(args)
    bad = [p for p in parse_deletions(sys.stdin.read()) if is_protected(p, pre)]
    for p in bad:
        print(pre + p)
    return 1 if bad else 0


def _subst(word, vars_):
    for k, v in vars_.items():
        word = word.replace("${%s}" % k, v).replace("$%s" % k, v)
    return word


def parse_deploy_rsyncs(text, vars_):
    """The --delete rsyncs of deploy_cpp.sh, with continuation lines joined."""
    joined, buf = [], ""
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        joined.append(buf + line)
        buf = ""
    if buf:
        joined.append(buf)
    step = ""
    result = []
    for line in joined:
        s = line.strip()
        if s.startswith("echo \"Step "):
            step = s[len("echo \""):].split(":")[0]
        if not s.startswith("rsync "):
            continue
        words = shlex.split(s, comments=True)
        if "--delete" not in words:
            continue
        opts, filters, pos = [], [], []
        i = 1
        while i < len(words):
            w = words[i]
            if w in ("--exclude", "--filter", "--include"):
                filters.append([w, _subst(words[i + 1], vars_)]); i += 2
                continue
            if w.startswith(("--exclude=", "--filter=", "--include=")):
                k, v = w.split("=", 1)
                filters.append([k, _subst(v, vars_)])
            elif w.startswith("-"):
                opts.append(w)
            else:
                pos.append(_subst(w, vars_))
            i += 1
        if len(pos) != 2:
            raise ValueError(f"cannot parse rsync (want 1 src + 1 dest): {s}")
        result.append({"step": step, "options": opts, "filters": filters,
                       "src": pos[0], "dest": pos[1]})
    return result


def _vars(args):
    v = {}
    i = 0
    while i < len(args):
        if args[i] == "--var":
            k, _, val = args[i + 1].partition("=")
            v[k] = val
            i += 2
        else:
            i += 1
    return v


def cmd_deploy_rsyncs(args):
    with open(args[0]) as f:
        specs = parse_deploy_rsyncs(f.read(), _vars(args[1:]))
    for s in specs:
        print(json.dumps(s))
    return 0


def dest_prefix(dest):
    """'magic@h:/opt/magic_dingus_box/magic_dingus_box_cpp/' -> 'magic_dingus_box_cpp/'."""
    path = dest.split(":", 1)[1] if ":" in dest else dest
    base = "/opt/magic_dingus_box/"
    path = path if path.endswith("/") else path + "/"
    return path[len(base):] if path.startswith(base) else path.lstrip("/")


def cmd_deploy_guard(args):
    script = args[0]
    print_only = "--print-only" in args
    log = args[args.index("--log") + 1] if "--log" in args else None
    with open(script) as f:
        specs = parse_deploy_rsyncs(f.read(), _vars(args[1:]))
    if not specs:
        die("no --delete rsync found in deploy_cpp.sh — refusing to guess", 1)
    violations = 0
    for s in specs:
        argv = ["rsync", "-n", "--itemize-changes"] + s["options"]
        for k, v in s["filters"]:
            argv += [k, v]
        argv += [s["src"], s["dest"]]
        print(f"[deploy-guard] {s['step'] or 'rsync'}: {shlex.join(argv)}")
        if print_only:
            continue
        r = subprocess.run(argv, capture_output=True, text=True)
        if log:
            with open(log, "a") as f:
                f.write(f"### {shlex.join(argv)}\n{r.stdout}{r.stderr}\n")
        if r.returncode != 0:
            print(f"[deploy-guard] rsync dry run failed (rc={r.returncode}): {r.stderr.strip()}")
            return 1
        pre = dest_prefix(s["dest"])
        dels = parse_deletions(r.stdout, pre)
        bad = [p for p in parse_deletions(r.stdout) if is_protected(p, pre)]
        print(f"[deploy-guard]   would delete {len(dels)} path(s) under {pre or '/'}; "
              f"{len(bad)} under data/ or config/")
        for p in bad:
            print(f"[deploy-guard]   PROTECTED: {pre}{p}")
        violations += len(bad)
    return 1 if violations else 0


def read_list(path):
    if not path:
        return []
    with open(path) as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]


def parse_field_state(text):
    """-> [(directive, arg)] ; raises ValueError on an unknown directive."""
    out = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        directive, _, arg = line.partition(" ")
        arg = arg.strip()
        if directive not in FIELD_DIRECTIVES:
            raise ValueError(f"line {n}: unknown directive {directive!r}")
        if directive == "pulse-client-conf":
            if arg:
                raise ValueError(f"line {n}: pulse-client-conf takes no argument")
        elif not arg:
            raise ValueError(f"line {n}: {directive} needs an argument")
        if directive in ("dropin", "file") and not arg.startswith("/"):
            raise ValueError(f"line {n}: {directive} path must be absolute: {arg}")
        if directive == "dropin" and not (arg.startswith(SYSTEMD_DIR + "/") and ".d/" in arg):
            raise ValueError(f"line {n}: not a drop-in under {SYSTEMD_DIR}: {arg}")
        if directive in ("unit", "old-unit") and "/" in arg and directive == "unit":
            raise ValueError(f"line {n}: unit takes a unit NAME, not a path: {arg}")
        out.append((directive, arg))
    return out


def field_plan(file_text, old_units=(), new_units=(), changed_units=(), old_dropins=(), new_dropins=()):
    """Merge the per-release file with git-derived lists into the strip plan.

    old/new_units: tree paths of unit files (e.g. magic_dingus_box_cpp/systemd/x.service)
    changed_units: tree paths present in both trees whose content differs
    old/new_dropins: '/etc/systemd/system/<unit>.d/<file>.conf' paths the trees' scripts write
    """
    plan = parse_field_state(file_text)
    unit_ext = (".service", ".timer", ".socket", ".path", ".mount")
    old_names = {os.path.basename(u) for u in old_units}
    for u in new_units:
        name = os.path.basename(u)
        if name.endswith(unit_ext) and name not in old_names:
            plan.append(("unit", name))
    for u in changed_units:
        if os.path.basename(u).endswith(unit_ext):
            plan.append(("old-unit", u))
    for d in new_dropins:
        if d not in set(old_dropins):
            plan.append(("dropin", d if d.startswith("/") else "/etc/" + d.lstrip("/")))
    seen, out = set(), []
    for d, a in plan:
        if (d, a) not in seen:
            seen.add((d, a))
            out.append((d, a))
    # stable: directive order first, then file order
    out.sort(key=lambda x: FIELD_ORDER[x[0]])
    return out


def cmd_field_plan(args):
    opts = {}
    i = 0
    while i < len(args):
        opts[args[i].lstrip("-")] = args[i + 1]
        i += 2
    if "file" not in opts:
        die("field-plan --file F [...]")
    with open(opts["file"]) as f:
        text = f.read()
    try:
        plan = field_plan(text, read_list(opts.get("old-units")), read_list(opts.get("new-units")),
                          read_list(opts.get("changed-units")), read_list(opts.get("old-dropins")),
                          read_list(opts.get("new-dropins")))
    except ValueError as e:
        die(f"{opts['file']}: {e}", 1)
    for d, a in plan:
        print(f"{d} {a}".rstrip())
    return 0


COMMANDS = {
    "manifest": cmd_manifest, "fingerprint": cmd_fingerprint, "syslisting": cmd_syslisting,
    "compare": cmd_compare, "check-tarball": cmd_check_tarball, "kiosk-idle": cmd_kiosk_idle,
    "rsync-deletions": cmd_rsync_deletions, "deletion-violations": cmd_deletion_violations,
    "deploy-rsyncs": cmd_deploy_rsyncs, "deploy-guard": cmd_deploy_guard,
    "field-plan": cmd_field_plan, "changed-keys": cmd_changed_keys, "get": cmd_get,
}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(2)
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]) or 0)
