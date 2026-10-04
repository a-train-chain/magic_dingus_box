#!/usr/bin/env python3
"""Lift the packaging steps out of .github/workflows/release.yml so the OTA
rehearsal builds its artifacts with the SAME shell the release job runs —
not a hand-copied approximation that silently drifts from CI.

Usage: extract_release_steps.py <release.yml> <version> <out_dir>

Writes into <out_dir>:
  source_NN_<slug>.sh   every build-source step that has a `run:` (minus the
                        GITHUB_REF/GITHUB_OUTPUT "Extract version" step),
                        with ${{ steps.version.outputs.VERSION }} substituted
  binary_inner.sh       the script build-arm64 runs inside `docker run ...
                        bash -c '<here>'`
  binary_image.txt      the image build-arm64 runs it in
  package_binary.sh     build-arm64's "Package binary" step
  release_body.md       the release job's release-notes body
  release_files.txt     the asset paths the release job uploads
"""
import re
import sys
from pathlib import Path

import yaml

VERSION_EXPR = re.compile(r"\$\{\{\s*steps\.version\.outputs\.VERSION\s*\}\}")


def sub(text: str, version: str) -> str:
    out = VERSION_EXPR.sub(version, text)
    if "${{" in out:
        raise SystemExit(f"unsubstituted GitHub expression left in:\n{out}")
    return out


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")[:40]


def main() -> int:
    wf_path, version, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    wf = yaml.safe_load(Path(wf_path).read_text())
    jobs = wf["jobs"]

    n = 0
    for step in jobs["build-source"]["steps"]:
        run = step.get("run")
        if not run or "GITHUB_OUTPUT" in run:
            continue
        n += 1
        (out_dir / f"source_{n:02d}_{slug(step.get('name', 'step'))}.sh").write_text(
            sub(run, version))

    build = next(s for s in jobs["build-arm64"]["steps"] if s.get("name") == "Build ARM64 binary")
    m = re.search(r"\n\s*(\S+)\s*\\\s*\n\s*bash -c '(.*)'\s*$", build["run"], re.S)
    if not m:
        raise SystemExit("could not find the `IMAGE bash -c '...'` shape in Build ARM64 binary")
    image, inner = m.group(1), m.group(2)
    if "'" in inner:
        raise SystemExit("inner build script contains a single quote; extraction would be wrong")
    (out_dir / "binary_image.txt").write_text(image + "\n")
    (out_dir / "binary_inner.sh").write_text(sub(inner, version))

    pkg = next(s for s in jobs["build-arm64"]["steps"] if s.get("name") == "Package binary")
    (out_dir / "package_binary.sh").write_text(sub(pkg["run"], version))

    rel = next(s for s in jobs["release"]["steps"] if s.get("name") == "Create Release")
    (out_dir / "release_body.md").write_text(sub(rel["with"]["body"], version))
    files = [ln.strip() for ln in sub(rel["with"]["files"], version).splitlines() if ln.strip()]
    (out_dir / "release_files.txt").write_text("\n".join(files) + "\n")
    print(f"extracted {n} build-source steps, binary build ({image}), package step, release body, {len(files)} assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
