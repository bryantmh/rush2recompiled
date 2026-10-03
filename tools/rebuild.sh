#!/bin/sh
# Regenerate recompiled code and rebuild. Run from the repo root in Git Bash.
set -e
rm -rf RecompiledFuncs
./.tools/N64Recomp.exe us.toml | grep -v "Indirect tail call" || true
./.tools/RSPRecomp.exe aspMain.us.toml
cmd //c "tools\vsenv.bat cmake --build build" > build.log 2>&1 || { grep -E " error|FAILED|undefined" build.log | head -30; exit 1; }
tail -1 build.log
