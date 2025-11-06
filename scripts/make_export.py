#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import stat
import tarfile
from datetime import datetime
from pathlib import Path


EXCLUDES_DIRS = {
    ".git",
    "dist",
    ".venv",
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
}

EXCLUDES_FILES_SUFFIX = {
    ".pyc",
    ".pyo",
}


def should_exclude(path: Path) -> bool:
    name = path.name
    if name in EXCLUDES_DIRS and path.is_dir():
        return True
    if any(name.endswith(suf) for suf in EXCLUDES_FILES_SUFFIX):
        return True
    return False


def copy_tree(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for item in src.iterdir():
        if item.name in EXCLUDES_DIRS:
            continue
        if should_exclude(item):
            continue
        target = dst / item.name
        if item.is_dir():
            shutil.copytree(item, target, dirs_exist_ok=True, ignore=ignore_patterns)
        else:
            shutil.copy2(item, target)


def ignore_patterns(dir: str, contents: list[str]):  # type: ignore[no-untyped-def]
    ignored: list[str] = []
    for name in contents:
        p = Path(dir) / name
        if should_exclude(p):
            ignored.append(name)
    return ignored


def write_run_script(out_dir: Path, default_audio: str) -> None:
    run_sh = out_dir / "run_linux_dev.sh"
    run_sh.write_text(
        """#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

SOCKET="/tmp/mpv-magic.sock"
AUDIO_DEVICE="${AUDIO_DEVICE:-%DEFAULT_AUDIO%}"
export DISPLAY="${DISPLAY:-:0}"
export MPV_SOCKET="$SOCKET"
export MAGIC_DATA_DIR="$PWD/dev_data"

python3 -m venv .venv >/dev/null 2>&1 || true
. .venv/bin/activate
pip -q install --upgrade pip
pip -q install -r requirements.txt || sudo apt-get install -y python3-pygame

pkill -f "mpv.*mpv-magic.sock" 2>/dev/null || true
rm -f "$SOCKET"

mpv --idle=yes --no-osc --no-osd-bar --keep-open=yes \
  --input-ipc-server="$MPV_SOCKET" \
  --audio-device="$AUDIO_DEVICE" \
  --vf=scale=720:480:force_original_aspect_ratio=increase,crop=720:480,setdar=4/3 \
  >/dev/null 2>&1 &

trap "pkill -f 'mpv.*mpv-magic.sock' 2>/dev/null || true; rm -f '$SOCKET'" EXIT

for i in {1..20}; do [ -S "$SOCKET" ] && break; sleep 0.25; done

python -m magic_dingus_box.main
""".replace("%DEFAULT_AUDIO%", default_audio),
        encoding="utf-8",
    )
    run_sh.chmod(run_sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def write_readme(out_dir: Path, default_audio: str) -> None:
    readme = out_dir / "README_RUN.md"
    readme.write_text(
        f"""Quick start on Linux (UConsole):
1) Install runtime libs:
   sudo apt-get update && sudo apt-get install -y mpv libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-ttf-2.0-0 fonts-dejavu-core python3-venv python3-pip
2) Extract this folder somewhere, then cd into it.
3) Optional: list audio devices: mpv --audio-device=help
4) Run (default audio: {default_audio}):
   ./run_linux_dev.sh
   # or override audio, e.g. HDMI:
   AUDIO_DEVICE='alsa/hdmi:CARD=vc4hdmi0,DEV=0' ./run_linux_dev.sh
""",
        encoding="utf-8",
    )


def make_tgz(folder: Path, tgz_out: Path) -> None:
    tgz_out.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tgz_out, "w:gz") as tar:
        tar.add(folder, arcname=folder.name)


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    dist_dir = repo_root / "dist"
    stamp = datetime.now().strftime("%Y%m%d_%H%M")
    out_dir = dist_dir / f"magic_dingus_box_export_{stamp}"
    default_audio = "alsa/sysdefault:CARD=RP1AudioOut"

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    copy_tree(repo_root, out_dir)

    write_run_script(out_dir, default_audio=default_audio)
    write_readme(out_dir, default_audio=default_audio)

    tgz_path = dist_dir / f"magic_dingus_box_{stamp}.tgz"
    make_tgz(out_dir, tgz_path)

    print(f"Staged export: {out_dir}")
    print(f"Archive: {tgz_path}")


if __name__ == "__main__":
    main()


