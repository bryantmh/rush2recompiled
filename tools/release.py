"""Build a commit and publish it as a GitHub release. Called by tools/hooks/pre-push when master is pushed.

Usage: py tools/release.py <remote> <sha> [<previous remote sha>]

The commit is exported to .release/src (only changed files are rewritten, so rebuilds stay incremental) and built
in .release/build, so uncommitted changes in the working tree never end up in a release. Submodules aren't exported;
lib is linked to the working tree's lib, which must be checked out at the commits the release commit records.

After a successful build the commit is pushed (the push that triggered the hook then has nothing left to do), and
the zipped game folder is uploaded as a release. VERSION holds the major.minor version and is only changed by hand: the
first release of a version is tagged v<major>.<minor>, and each later one gets the next patch number, up to .999.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / ".release"
SRC = WORK / "src"
BUILD = WORK / "build"
PACKAGE_FILES = ["Rush2Recompiled.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll", "recompcontrollerdb.txt"]
ROM_FILES = ["rush2.us.z64", "rush2.us.recomp.z64"]
NULL_SHA = "0" * 40
MAX_PATCH = 999


def git(*args, cwd=ROOT):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def run(cmd, cwd):
    print(f"> {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def check_submodules(sha):
    for line in git("ls-tree", "-r", sha, "lib").splitlines():
        mode, kind, commit, path = line.split(maxsplit=3)
        if kind != "commit":
            continue
        actual = git("rev-parse", "HEAD", cwd=ROOT / path)
        if actual != commit:
            raise SystemExit(f"{path} is at {actual[:7]} but {sha[:7]} records {commit[:7]}; "
                             f"run `git submodule update` or commit the submodule change first")


def export(sha):
    """Writes the commit's files into SRC, leaving unchanged files (and their timestamps) alone."""
    archive = subprocess.run(["git", "archive", "--format=tar", sha], cwd=ROOT, check=True, capture_output=True).stdout
    wanted = set()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            path = SRC / member.name
            wanted.add(path)
            data = tar.extractfile(member).read()
            if path.is_file() and path.read_bytes() == data:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    # Remove files the commit no longer has, skipping generated and linked folders.
    keep = {SRC / "lib", SRC / "RecompiledFuncs", SRC / ".tools"} | {SRC / name for name in ROM_FILES}
    for dirpath, dirnames, filenames in os.walk(SRC, topdown=False):
        folder = Path(dirpath)
        if any(folder == k or k in folder.parents for k in keep):
            continue
        for name in filenames:
            path = folder / name
            if path not in wanted and path not in keep:
                path.unlink()
        if folder != SRC and not any(folder.iterdir()):
            folder.rmdir()

    lib = SRC / "lib"
    if not lib.exists():
        subprocess.run(["cmd", "/c", "mklink", "/J", str(lib), str(ROOT / "lib")], check=True, capture_output=True)
    for name in ROM_FILES:
        if not (ROOT / name).is_file():
            raise SystemExit(f"{name} is missing; run tools/extract.py first")
        shutil.copy2(ROOT / name, SRC / name)
    shutil.copytree(ROOT / ".tools", SRC / ".tools", dirs_exist_ok=True)


def build():
    shutil.rmtree(SRC / "RecompiledFuncs", ignore_errors=True)
    print("> N64Recomp us.toml", flush=True)
    recomp = subprocess.run([str(SRC / ".tools/N64Recomp.exe"), "us.toml"], cwd=SRC, capture_output=True, text=True)
    if recomp.returncode != 0:
        print(recomp.stdout, recomp.stderr)
        raise SystemExit("N64Recomp failed")
    run([str(SRC / ".tools/RSPRecomp.exe"), "aspMain.us.toml"], SRC)

    vsenv = str(ROOT / "tools/vsenv.bat")
    if not (BUILD / "build.ninja").exists():
        run(["cmd", "/c", vsenv, "cmake", "-S", str(SRC), "-B", str(BUILD), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"], ROOT)
    run(["cmd", "/c", vsenv, "cmake", "--build", str(BUILD), "--target", "Rush2Recompiled"], ROOT)


def package(name):
    zip_path = WORK / f"{name}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for file in PACKAGE_FILES:
            z.write(BUILD / file, f"Rush2Recompiled/{file}")
        for path in sorted((BUILD / "assets").rglob("*")):
            if path.is_file():
                z.write(path, f"Rush2Recompiled/{path.relative_to(BUILD)}")
    return zip_path


def github_repo(remote):
    url = git("remote", "get-url", remote)
    m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?/?$", url)
    if not m:
        raise SystemExit(f"Remote {remote} ({url}) isn't a GitHub repo")
    return m.group(1)


def next_tag(repo, sha):
    """Returns the tag for the next release of the major.minor version in the commit's VERSION file."""
    base = git("show", f"{sha}:VERSION")
    if not re.fullmatch(r"\d+\.\d+", base):
        raise SystemExit(f"VERSION must be <major>.<minor>, not {base!r}")
    tags = subprocess.run(["gh", "release", "list", "--repo", repo, "--limit", "10000", "--json", "tagName",
                           "--jq", ".[].tagName"], check=True, capture_output=True, text=True).stdout.split()
    patches = [int(m.group(1) or 0) for t in tags if (m := re.fullmatch(rf"v{re.escape(base)}(?:\.(\d+))?", t))]
    if not patches:
        return f"v{base}"
    patch = max(patches) + 1
    if patch > MAX_PATCH:
        raise SystemExit(f"v{base} has used all {MAX_PATCH} patch versions; bump the minor or major version in VERSION")
    return f"v{base}.{patch}"


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    remote, sha = sys.argv[1], git("rev-parse", sys.argv[2])
    previous = sys.argv[3] if len(sys.argv) == 4 and sys.argv[3] != NULL_SHA else None
    repo = github_repo(remote)
    short = sha[:7]
    tag = next_tag(repo, sha)

    check_submodules(sha)
    SRC.mkdir(parents=True, exist_ok=True)
    export(sha)
    build()
    zip_path = package(f"Rush2Recompiled-{tag[1:]}")

    log_range = [f"{previous}..{sha}"] if previous else ["-1", sha]
    changes = git("log", "--format=- %s", *log_range)
    notes = (f"Unzip anywhere and run `Rush2Recompiled.exe`. You'll need your own Rush 2: Extreme Racing USA ROM.\n\n"
             f"## Changes\n\n{changes}\n")

    # Push the commit now so the release tag can point at it; the outer push then finds master up to date.
    run(["git", "push", "--no-verify", remote, f"{sha}:refs/heads/master"], ROOT)
    run(["gh", "release", "create", tag, str(zip_path), "--repo", repo, "--target", sha,
         "--title", f"Build {tag[1:]} ({short})", "--notes", notes, "--latest"], ROOT)
    zip_path.unlink()


if __name__ == "__main__":
    main()
