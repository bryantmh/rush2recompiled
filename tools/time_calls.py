#!/usr/bin/env python3
"""Times the calls a recompiled function makes, to find where a slow game frame spends its time (test aid).

    py tools/time_calls.py apply <func> [<func> ...]   instrument every call statement inside each function
    py tools/time_calls.py apply --copy <dir> <func> ...   write instrumented copies of the files to <dir> instead
    py tools/time_calls.py revert                      put the original RecompiledFuncs files back

<func> is a recompiled function name or its address (race_start_timer_800AE670 or 800AE670). After `apply`, build
and run with RUSH2_TIME_CALLS=<log file>: each call that takes longer than RUSH2_TIME_CALLS_MS (default 2) ms is
logged as "<UTC seconds> <caller> -> <callee> <ms>" (appended, so delete the log before a run; each
instrumented file opens it). Instrument the slow callee next to narrow it down.
The originals are kept in tmp/time_calls/ until `revert`; regenerating RecompiledFuncs also removes the timing.
With --copy the tree is left alone (other sessions regenerate RecompiledFuncs): compile the copies with the command
`ninja -t commands` gives for CMakeFiles/RecompiledFuncs.dir/RecompiledFuncs/funcs_N.c.obj and link those objects
ahead of RecompiledFuncs.lib, which then leaves its own copies out.
"""
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUNCS = ROOT / "RecompiledFuncs"
BACKUP = ROOT / "tmp" / "time_calls"
MARK = "/* time_calls */"

HELPERS = MARK + r"""
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double time_calls_now(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static void time_calls_report(const char* caller, const char* callee, double start) {
    static FILE* f = NULL; static int init = 0; static double min_ms = 2.0;
    if (!init) { const char* p = getenv("RUSH2_TIME_CALLS"); const char* m = getenv("RUSH2_TIME_CALLS_MS"); f = p ? fopen(p, "a") : NULL; if (m) min_ms = atof(m); init = 1; }
    double ms = time_calls_now() - start;
    if (f && ms >= min_ms) { fprintf(f, "%.3f %s -> %s %.2f\n", start / 1000.0, caller, callee, ms); fflush(f); }
}
"""


def find(name):
    if re.fullmatch(r"(0x)?[0-9A-Fa-f]{8}", name):
        pattern = re.compile(r"^RECOMP_FUNC void (\w+_%s)\(" % name[-8:].upper(), re.M)
    else:
        pattern = re.compile(r"^RECOMP_FUNC void (%s)\(" % re.escape(name), re.M)
    for path in sorted(FUNCS.glob("funcs_*.c")):
        m = pattern.search(path.read_text(encoding="utf-8"))
        if m:
            return path, m.group(1)
    sys.exit(f"{name}: not found in RecompiledFuncs")


def apply(names, copy_dir=None):
    for name in names:
        path, func = find(name)
        if copy_dir is not None:
            copy_dir.mkdir(parents=True, exist_ok=True)
            out = copy_dir / path.name
            if not out.exists():
                shutil.copy(path, out)
        else:
            BACKUP.mkdir(parents=True, exist_ok=True)
            if not (BACKUP / path.name).exists():
                shutil.copy2(path, BACKUP / path.name)
            out = path
        text = out.read_text(encoding="utf-8")
        if MARK not in text:
            text = HELPERS + text
        start = text.index(f"RECOMP_FUNC void {func}(")
        end = text.index("\n;}\n", start)  # generated functions end with ";}"
        body = text[start:end]
        call = re.compile(r"^(\s+)(\w+)\(rdram, ctx\);$", re.M)
        body, count = call.subn(lambda m: f'{m.group(1)}{{ double tc_ = time_calls_now(); {m.group(2)}(rdram, ctx); '
                                f'time_calls_report("{func}", "{m.group(2)}", tc_); }}', body)
        out.write_text(text[:start] + body + text[end:], encoding="utf-8")
        print(f"{func}: {count} calls timed ({out})")


def revert():
    if not BACKUP.exists():
        return
    for saved in BACKUP.glob("funcs_*.c"):
        shutil.copy(saved, FUNCS / saved.name)  # a new mtime, so the build recompiles it
        print(f"restored {saved.name}")
    shutil.rmtree(BACKUP)


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1:3] == ["apply", "--copy"]:
        apply(sys.argv[4:], Path(sys.argv[3]).resolve())
    elif len(sys.argv) >= 3 and sys.argv[1] == "apply":
        apply(sys.argv[2:])
    elif len(sys.argv) == 2 and sys.argv[1] == "revert":
        revert()
    else:
        sys.exit(__doc__)
