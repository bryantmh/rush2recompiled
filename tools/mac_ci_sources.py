"""Hands the ROM-derived sources to the macOS CI build (.github/workflows/macos.yml) without making them public.

Usage: py tools/mac_ci_sources.py upload     (on the PC that has the ROM, after N64Recomp us.toml and RSPRecomp)
       python3 tools/mac_ci_sources.py download  (in CI; needs GH_TOKEN with contents: write to see draft releases)

RecompiledFuncs/ and rsp/aspMain.cpp are generated from the ROM, so they can't be committed to the public repo. upload
packs them into recomp-sources-<key>.tar.gz and attaches it to the draft release ci-recomp-sources (a draft is only
visible to the repo's collaborators and is never published). <key> hashes the recompilers' inputs (us.toml, the symbol
file and aspMain.us.toml, line endings ignored), so CI picks the pack made from the same inputs as the commit it
builds, and fails with a clear message when nobody uploaded one. Notes: docs/macos.md.
"""
import hashlib
import io
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RELEASE = "ci-recomp-sources"
INPUTS = ["us.toml", "syms/rush2.us.syms.toml", "aspMain.us.toml"]
OUTPUTS = ["RecompiledFuncs", "rsp/aspMain.cpp"]


def key():
    h = hashlib.sha256()
    for name in INPUTS:
        h.update(name.encode())
        h.update((ROOT / name).read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]


def asset_name():
    return f"recomp-sources-{key()}.tar.gz"


def gh(*args, check=True):
    return subprocess.run(["gh", *args], cwd=ROOT, check=check, capture_output=True, text=True)


def upload():
    newest_input = max((ROOT / name).stat().st_mtime for name in INPUTS)
    for name in OUTPUTS:
        path = ROOT / name
        if not path.exists():
            raise SystemExit(f"{name} is missing: run N64Recomp us.toml and RSPRecomp aspMain.us.toml first")
        stamp = max(p.stat().st_mtime for p in path.rglob("*")) if path.is_dir() else path.stat().st_mtime
        if stamp < newest_input:
            raise SystemExit(f"{name} is older than the recompiler inputs: regenerate it first")

    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        for name in OUTPUTS:
            tar.add(ROOT / name, arcname=name)
    out = ROOT / "tmp" / asset_name()
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(data.getvalue())

    if gh("release", "view", RELEASE, check=False).returncode != 0:
        gh("release", "create", RELEASE, "--draft", "--title", "CI recompiler sources (private, never publish)",
           "--notes", "Draft only: holds the ROM-derived sources the macOS CI build needs (tools/mac_ci_sources.py).")
    gh("release", "upload", RELEASE, str(out), "--clobber")
    print(f"uploaded {out.name} ({len(data.getvalue()) // 1024} KB)")


def download():
    name = asset_name()
    result = gh("release", "download", RELEASE, "--pattern", name, "--dir", str(ROOT / "tmp"), "--clobber", check=False)
    if result.returncode != 0:
        raise SystemExit(f"No {name} on the {RELEASE} draft release ({result.stderr.strip()}). On the PC with the ROM, "
                         f"regenerate RecompiledFuncs for this commit and run: py tools/mac_ci_sources.py upload")
    with tarfile.open(ROOT / "tmp" / name) as tar:
        tar.extractall(ROOT, filter="data")
    print(f"extracted {name}")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("upload", "download"):
        raise SystemExit(__doc__)
    upload() if sys.argv[1] == "upload" else download()
