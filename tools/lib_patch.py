#!/usr/bin/env python3
"""Keeps this project's changes to the lib/ submodules as patch files that commit with the project.

A submodule's commits live in its own repository, so an edit made inside lib/<name> can't be committed here. Each
edited submodule gets lib/patches/<name>.patch instead: the diff of its working tree against the commit the project
pins. CMakeLists.txt applies the patches when it configures, so a fresh checkout builds with them.

    python tools/lib_patch.py save [name ...]   write the patch of each submodule (all of them by default) from its
                                                working tree; a submodule with no changes has its patch removed
    python tools/lib_patch.py status            list the submodules whose working tree differs from their patch

Run `save` after editing anything under lib/<name> and commit the patch with the change that needs it. Files added to
a submodule are picked up too. A nested submodule's patch is named after its path under lib/ with "--" for each "/"
(lib/RecompFrontend/recompui/lib/RmlUi: RecompFrontend--recompui--lib--RmlUi.patch); give that name to `save` too.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCHES = ROOT / "lib" / "patches"


def git(lib, *args):
    return subprocess.run(["git", "-C", str(lib), *args], check=True, capture_output=True).stdout


def submodule_paths(repo):
    if not (repo / ".gitmodules").exists():
        return []
    try:
        out = git(repo, "config", "--file", ".gitmodules", "--get-regexp", r"\.path$").decode()
    except subprocess.CalledProcessError:
        return []
    return [repo / line.split()[1] for line in out.splitlines()]


def patch_name(lib):
    return "--".join(lib.relative_to(ROOT / "lib").parts)


def submodules():
    """The submodules in lib/ and, checked out, the submodules nested in them."""
    found = []
    pending = [p for p in submodule_paths(ROOT) if p.parent == ROOT / "lib"]
    while pending:
        lib = pending.pop(0)
        found.append(lib)
        pending += [p for p in submodule_paths(lib) if (p / ".git").exists()]
    return found


def diff(lib):
    # Untracked files are marked intent-to-add so the diff includes them.
    git(lib, "add", "--intent-to-add", "--all")
    return git(lib, "diff", "--binary", "--ignore-submodules=all")


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("save", "status"):
        sys.exit(__doc__)

    libs = submodules()
    if args[1:]:
        libs = [lib for lib in libs if patch_name(lib) in args[1:]]
        missing = set(args[1:]) - {patch_name(lib) for lib in libs}
        if missing:
            sys.exit(f"not a submodule in lib/: {', '.join(sorted(missing))}")

    for lib in libs:
        patch = PATCHES / f"{patch_name(lib)}.patch"
        new = diff(lib)
        old = patch.read_bytes() if patch.exists() else b""
        if new == old:
            continue
        if args[0] == "status":
            print(f"{patch_name(lib)}: working tree differs from lib/patches/{patch.name}")
        elif new:
            PATCHES.mkdir(exist_ok=True)
            patch.write_bytes(new)
            print(f"wrote lib/patches/{patch.name}")
        else:
            patch.unlink()
            print(f"removed lib/patches/{patch.name}")


if __name__ == "__main__":
    main()
