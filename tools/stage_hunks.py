"""Stages only the hunks of a file whose changed lines match a pattern (parallel sessions share the working tree, so a
file can hold several sessions' changes).

Usage: python tools/stage_hunks.py FILE REGEX [REGEX...]   stage the matching hunks
       python tools/stage_hunks.py --dry FILE REGEX...      list which hunks would be staged
A hunk is staged when any of its added or removed lines matches any REGEX. Hunks are taken with no context lines, so
neighbouring changes from different sessions stay separate.
"""
import re, subprocess, sys

def main():
    args = sys.argv[1:]
    dry = args and args[0] == '--dry'
    if dry:
        args = args[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    path, pats = args[0], [re.compile(p) for p in args[1:]]
    # Bytes throughout: the files keep their own line endings (CRLF in the index for some).
    diff = subprocess.run(['git', 'diff', '-U0', '--', path], capture_output=True, check=True).stdout
    lines = diff.decode('utf-8', 'surrogateescape').splitlines(keepends=True)
    head, hunks, cur = [], [], None
    for line in lines:
        if line.startswith('@@'):
            cur = [line]
            hunks.append(cur)
        elif cur is None:
            head.append(line)
        else:
            cur.append(line)
    keep = [h for h in hunks if any(p.search(l[1:]) for l in h[1:] if l[:1] in '+-' for p in pats)]
    for h in hunks:
        print(('stage ' if h in keep else 'skip  ') + h[0].strip()[:100])
    if dry or not keep:
        return
    patch = ''.join(head) + ''.join(''.join(h) for h in keep)
    subprocess.run(['git', 'apply', '--cached', '--unidiff-zero', '--whitespace=nowarn', '-'],
                   input=patch.encode('utf-8', 'surrogateescape'), check=True)

if __name__ == '__main__':
    main()
