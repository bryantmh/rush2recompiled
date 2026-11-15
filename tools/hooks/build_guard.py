"""Claude Code PreToolUse hook (.claude/settings.json): blocks raw builds of build/ from the Bash and PowerShell tools.

Sessions share build/. A raw `cmake --build build` or `ninja` there runs beside other sessions' builds, damages
build/.ninja_log (forcing full rebuilds) and fails the link when a game holds the exe. tools/build.py takes the
shared build lock, skips work another build already did and renames a running exe instead of waiting.
Reads the hook JSON on stdin; exit 2 with a message on stderr blocks the call.
"""
import json, re, sys

RAW_BUILD = re.compile(
    r'''cmake(?:\.exe)?["']?\s+--build\s+["']?(?:\./)?build["']?(?:\s|$)'''  # cmake --build build
    r'''|ninja(?:\.exe)?["']?\s+(?:[^|;&\n]*\s)?-C\s+["']?(?:\./)?build["']?(?:\s|$)''',  # ninja -C build
    re.I)


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return 0
    cmd = (data.get('tool_input') or {}).get('command') or ''
    if RAW_BUILD.search(cmd):
        sys.stderr.write(
            'Blocked: build with `python tools/build.py` (add --target T for one target, --regen to force '
            'N64Recomp), not a raw cmake/ninja build of build/. It shares one lock with every session, skips work '
            'another build already did, and renames the exe if a game is running instead of waiting.\n')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
