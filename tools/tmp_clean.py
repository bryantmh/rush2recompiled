#!/usr/bin/env python3
"""Frees space in tmp/ by deleting the game data folders that test runs leave behind.

A test run (tools/test_race.py, drone_pace.py, trackview/runs.py ...) gives the game a data folder of its own
(RUSH2_DATA_DIR): ROMs hard-linked from build/, mods linked or copied, its own saves, settings and caches. Once the run is
done the folder is only worth its disk space. This finds every such folder under tmp/ (one holding rush2.n64.us.z64) and
deletes the ones nobody has used for --hours (default 2), so a session's runs in progress are left alone.

Junctions and links (build/mods entries linked in by test_race.py) are unlinked, never followed, so nothing in build/
is touched. Hard-linked ROMs only lose this folder's link.

  python tools/tmp_clean.py            list what would go and its size
  python tools/tmp_clean.py --apply    delete it
  --hours N   keep folders used in the last N hours
  --all       every folder under tmp/ untouched for --hours, not just data folders (whole scratch cleanup)
"""
import argparse
import os
import stat
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(ROOT, 'tmp')
#: tmp/ paths tools use as fixed caches or outputs: never removed by --all.
KEEP = {'trackview', 'stock_tracks', 'linux', 'macos', 'hud_preview', 'issues', 'build.log', 'build.lock.info',
        'build.lock'}


def is_link(path):
    return os.path.islink(path) or (hasattr(os.path, 'isjunction') and os.path.isjunction(path))


def newest(path, limit):
    """Newest modification time in the tree (links not followed), stopping early once one is newer than limit."""
    best = os.lstat(path).st_mtime
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if not is_link(os.path.join(dirpath, d))]
        for n in dirnames + filenames:
            try:
                best = max(best, os.lstat(os.path.join(dirpath, n)).st_mtime)
            except OSError:
                pass
            if best > limit:
                return best
    return best


def size(path):
    total, seen = 0, set()
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if not is_link(os.path.join(dirpath, d))]
        for n in filenames:
            try:
                st = os.lstat(os.path.join(dirpath, n))
            except OSError:
                continue
            if st.st_nlink > 1:
                continue        # hard-linked from build/: deleting it frees nothing
            total += st.st_size
    return total


def remove(path):
    if is_link(path):
        os.rmdir(path) if os.path.isdir(path) else os.unlink(path)
        return
    if os.path.isdir(path):
        for n in os.listdir(path):
            remove(os.path.join(path, n))
        os.rmdir(path)
        return
    try:
        os.unlink(path)
    except PermissionError:
        os.chmod(path, stat.S_IWRITE)
        os.unlink(path)


def data_folders():
    out = []
    for dirpath, dirnames, filenames in os.walk(TMP):
        dirnames[:] = [d for d in dirnames if not is_link(os.path.join(dirpath, d))]
        if 'rush2.n64.us.z64' in filenames:
            out.append(dirpath)
            dirnames[:] = []
        elif os.path.relpath(dirpath, TMP).count(os.sep) >= 3:
            dirnames[:] = []
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--hours', type=float, default=2)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    limit = time.time() - a.hours * 3600
    if a.all:
        targets = [os.path.join(TMP, n) for n in os.listdir(TMP) if n not in KEEP]
    else:
        targets = data_folders()
    freed = 0
    for t in sorted(targets):
        if newest(t, limit) > limit:
            print('in use  ', os.path.relpath(t, ROOT))
            continue
        s = size(t)
        freed += s
        print('%-8s %7.2f GB  %s' % ('delete' if a.apply else 'would', s / 1e9, os.path.relpath(t, ROOT)))
        if a.apply:
            try:
                remove(t)
            except OSError as e:
                print('  failed:', e, file=sys.stderr)
    print('%s %.1f GB' % ('freed' if a.apply else 'would free', freed / 1e9))


if __name__ == '__main__':
    main()
