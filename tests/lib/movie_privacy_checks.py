"""Run real delivery/clone sections only inside disposable fixtures."""
from pathlib import Path
import os
import re
import shutil
import shlex
import subprocess
import sys
import tempfile

NAMES = (
    "movie_library.json", "movie_library.json.tmp",
    "qbit_paused_by_kiosk", "qbit_paused_by_kiosk.tmp",
    "qbit_paused_by_kiosk.services", "qbit_paused_by_kiosk.services.tmp",
    "qbit_paused_by_kiosk.services.claims", "qbit_paused_by_kiosk.services.claims.tmp",
)
COMMITTED = ("qbit_paused_by_kiosk", "qbit_paused_by_kiosk.services",
             "qbit_paused_by_kiosk.services.claims")
ROOT = Path(sys.argv[2]).resolve()
GI = ROOT / "scripts/golden_image"


def read(path):
    return (ROOT / path).read_text()


def seed(data, value="source-owner"):
    data.mkdir(parents=True, exist_ok=True)
    for name in NAMES:
        (data / name).write_text(f'{value}: {name}\n')
    (data / "public.json").write_text("shipped fixture\n")
    (data / "owner_settings.json").write_text("owner preference\n")


def absent(data, names=NAMES):
    for name in names:
        assert not (data / name).exists(), f"private runtime file survived: {data / name}"


def run(args, **kw):
    subprocess.run(args, check=True, **kw)


def sync_filters(t):
    src = read("magic_dingus_box_cpp/scripts/update.sh")
    blocks = re.findall(r"rsync -[a-z]+\s+--delete[^\n]*\n((?:\s*--(?:exclude|include)\s+'[^']+'[^\n]*\n)+)", src)
    assert len(blocks) == 4
    deploy = read("magic_dingus_box_cpp/scripts/deploy_cpp.sh")
    deploy = re.search(r"^rsync -avz --checksum .*?(?=\n\n)", deploy, re.M | re.S).group()
    # Exercise each production filter list with real rsync --delete. Existing
    # destination bytes must survive; private source bytes must never arrive.
    for i, (body, prefix) in enumerate([(b, "magic_dingus_box_cpp/") for b in blocks] + [(deploy, "")]):
        a, b, empty = (t / str(i) / n for n in ("src", "dst", "empty"))
        seed(a / prefix / "data")
        seed(b / prefix / "data", "local-owner")
        empty.mkdir(parents=True)
        filters = []
        for kind, value in re.findall(r"--(exclude|include|filter)\s+'([^']+)'", body):
            filters.extend([f"--{kind}", value])
        run(["rsync", "-a", "--delete", *filters, f"{a}/", f"{b}/"])
        for name in NAMES:
            want = f"local-owner: {name}\n"
            assert (b / prefix / "data" / name).read_text() == want, f"sync {i + 1} clobbered {name}"
        run(["rsync", "-a", "--delete", *filters, f"{a}/", f"{empty}/"])
        absent(empty / prefix / "data")
        assert (empty / prefix / "data/public.json").exists()


def source_ignore(t):
    paths = [f"magic_dingus_box_cpp/data/{n}" for n in NAMES]
    result = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "--no-index", "--stdin"],
                            input="\n".join(paths + ["magic_dingus_box_cpp/data/public.json"]),
                            text=True, capture_output=True)
    assert result.returncode in (0, 1), result.stderr
    assert set(result.stdout.splitlines()) == set(paths), result.stdout


def release(t):
    src = read(".github/workflows/release.yml").split("- name: Create source archive", 1)[1]
    src = src.split("\n      - name:", 1)[0]
    filters = re.findall(r"--exclude='([^']+)'", src)
    assert "data/media_info.json" in filters  # extraction guard
    tree = t / "tree"
    seed(tree / "magic_dingus_box_cpp/data")
    seed(tree / "magic_dingus_box_cpp/build/data")
    out = t / "release.tar.gz"
    run(["tar", "-czf", str(out), *[f"--exclude={f}" for f in filters], "."], cwd=tree)
    dest = t / "unpacked"
    dest.mkdir()
    run(["tar", "-xzf", str(out), "-C", str(dest)])
    absent(dest / "magic_dingus_box_cpp/data")
    absent(dest / "magic_dingus_box_cpp/build/data")
    assert (dest / "magic_dingus_box_cpp/data/public.json").exists()


def staging(t):
    src = read("magic_dingus_box_cpp/CMakeLists.txt")
    blocks = re.findall(r"file\(COPY \$\{CMAKE_SOURCE_DIR\}/(?:data|dev_data) DESTINATION \$\{CMAKE_BINARY_DIR\}.*?\n\s*\)", src, re.S)
    assert len(blocks) == 2
    for name in ("data", "dev_data"):
        seed(t / "source" / name)
    script = t / "copy.cmake"
    script.write_text(f'set(CMAKE_SOURCE_DIR "{t}/source")\nset(CMAKE_BINARY_DIR "{t}/stage")\n' + "\n".join(blocks))
    run(["cmake", "-P", str(script)])
    for name in ("data", "dev_data"):
        absent(t / "stage" / name)
        assert (t / "stage" / name / "public.json").exists()


def os_bash():
    # Live-clone uses Bash4 features supported on the Pi. Prefer an explicitly
    # supplied interpreter so this fixture also executes the production pass.
    return os.environ.get("MDB_TEST_BASH", shutil.which("bash"))


# The one repository file the extracted sections may name: sourced, read
# only. The checkout itself can live under /home (CI runners) or /opt (a
# box), so it is exempted by exact path, never by its directory.
LIBRARY = shlex.quote(str(GI / "clone_stash_lib.sh"))


def shell(t, body):
    script = t / "run.sh"
    # Pi stash code uses GNU stat -c; translate that command-line boundary
    # on macOS while still reading real fixture modes/uid/gid.
    stat_adapter = ""
    if sys.platform == "darwin":
        stat_adapter = '''stat() {
    if [[ "$1" == -c ]]; then
        case "$2" in
            %a) command stat -f %Lp "$3" ;;
            %u) command stat -f %u "$3" ;;
            %g) command stat -f %g "$3" ;;
            *) return 1 ;;
        esac
    else
        command stat "$@"
    fi
}
'''
    script.write_text("set -euo pipefail\nlog() { :; }\n" + stat_adapter + body)
    # Never execute a host/Pi path: fixture paths replace every production
    # absolute path before any extracted deletion or stash command can run.
    code = "\n".join(l for l in script.read_text().splitlines() if not l.lstrip().startswith("#"))
    scan = code.replace(LIBRARY, "LIBRARY").replace(str(t), "FIXTURE")
    assert not re.search(r'(?<![\w/])/(?:opt|home|root|var|etc|boot|run)/', scan), "unsafe host path in fixture"
    run([os_bash(), str(script)])


def clone(t):
    box = t / "box"
    seed(box / "data")
    seed(box / "build/data")
    for i, name in enumerate(COMMITTED):
        (box / "data" / name).chmod(0o600 + i * 0o20)
    before = {n: ((box / "data" / n).read_bytes(), (box / "data" / n).stat().st_mode & 0o777)
              for n in COMMITTED}
    prepare = read("scripts/golden_image/prepare_for_cloning.sh")
    # Assignment/append section, curation section, then the real verified
    # copy-and-shred pass; omit every root-only service/disk operation.
    lists = prepare[prepare.index("ARR_APPS=("):prepare.index("SHIP_PLAYLISTS=(")]
    lists = lists.replace("/opt/magic_dingus_box/magic_dingus_box_cpp", str(box))
    for absolute in ("/opt/", "/home/", "/root/", "/var/", "/etc/", "/boot/", "/run/"):
        lists = lists.replace(absolute, f"{t}/unused{absolute}")
    curation = prepare[prepare.index("SHIP_PLAYLISTS=("):prepare.index("unset _i _store _pruned _stashed") + len("unset _i _store _pruned _stashed")]
    end = 'log "[2c/5] ${stripped} application secret file(s) removed (stash: ${SECRET_STASH})"'
    stash = prepare[prepare.index("_expanded=()"):prepare.index(end) + len(end)]
    lib = shlex.quote(str(GI / "clone_stash_lib.sh"))
    shell(t, f'''source {lib}
CPP_DIR={shlex.quote(str(box))}
DATA_DIR="$CPP_DIR/data"
SECRET_STASH={shlex.quote(str(t / 'stash'))}
MDB_CONTENT_STASH_DIR={shlex.quote(str(t / 'content-stash'))}
mdb_check_stash_drive() {{ echo ok; }}
mdb_init_secret_stash "$SECRET_STASH"
stash_or_abort() {{ mdb_stash_copy "$1" "$SECRET_STASH"; }}
{lists}
{curation}
declare -p SECRET_PATHS > {shlex.quote(str(t / "paths.sh"))}
''')
    absent(box / "data", ("movie_library.json", "movie_library.json.tmp"))
    absent(box / "build/data")
    shell(t, f'''source {lib}
SECRET_STASH={shlex.quote(str(t / "stash"))}
stash_or_abort() {{ mdb_stash_copy "$1" "$SECRET_STASH"; }}
source {shlex.quote(str(t / "paths.sh"))}
{stash}
''')
    absent(box / "data")
    absent(box / "build/data")
    assert (box / "data/owner_settings.json").read_text() == "owner preference\n"
    # The existing manifest restore runs before services restart in restore.
    shell(t, f'source {lib}\nmdb_copy_back_stash {shlex.quote(str(t / "stash"))}\n')
    for name in COMMITTED:
        restored = box / "data" / name
        assert (restored.read_bytes(), restored.stat().st_mode & 0o777) == before[name], f"lost source ownership: {name}"
    absent(box / "data", tuple(n for n in NAMES if n not in COMMITTED))
    absent(box / "build/data")


def first_boot(t):
    install = t / "install"
    box = install / "magic_dingus_box_cpp"
    for data in (box / "data", box / "build/data"):
        seed(data)
    for data in (box / "data", box / "build/data"):
        (data / "media_browser.db").touch()
    key = t / "unused/home/magic/.config/magic_dingus_box/tmdb_api_key"
    key.parent.mkdir(parents=True)
    key.touch()
    src = read("scripts/golden_image/first_boot.sh")
    body = re.search(r"^step_wipe_personal_data\(\) \{\n.*?^\}", src, re.M | re.S).group()
    body = body.replace("/home/magic/", str(t / "unused/home/magic") + "/")
    shell(t, f'INSTALL_DIR={shlex.quote(str(install))}\nDATA_DIR="$INSTALL_DIR/magic_dingus_box_cpp/data"\nBUILD_DATA_DIR="$INSTALL_DIR/magic_dingus_box_cpp/build/data"\n{body}\nstep_wipe_personal_data\n')
    for data in (box / "data", box / "build/data"):
        absent(data)
        assert (data / "owner_settings.json").exists()


def golden(t):
    box = t / "box"
    for data in (box / "data", box / "build/data"):
        seed(data)
    src = read("scripts/golden_image/prepare_golden_image.sh")
    # Run the complete existing Step 4 data cleanup boundary (no root paths).
    body = src.split('di_count=0\n', 1)[1].split('# Step 5:', 1)[0]
    shell(t, f'INSTALL_DIR={shlex.quote(str(box))}\nCPP_DIR="$INSTALL_DIR"\nDATA_DIR="$CPP_DIR/data"\nRED=\nGREEN=\nDIM=\nNC=\ndi_count=0\n{body}\n')
    for data in (box / "data", box / "build/data"):
        absent(data)
        assert (data / "owner_settings.json").exists()


with tempfile.TemporaryDirectory(prefix="mdb-movie-privacy-") as tmp:
    globals()[sys.argv[1]](Path(tmp))
