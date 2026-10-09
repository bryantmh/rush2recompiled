"""Links a private copy of Rush2Recompiled.exe from build/'s current objects (test aid).

Usage: python tools/link_copy.py OUT_DIR

For when build/Rush2Recompiled.exe can't be relinked: another session's game holds it, or another session's
unfinished edits stop `cmake --build` before the link. Compile the objects you changed first (e.g.
`cmake --build build --target CMakeFiles/Rush2Recompiled.dir/src/x.cpp.obj`); the others are linked as they last
built. Writes the linker response file ninja would (from build/build.ninja), runs the link line from
`ninja -t commands` with /out, /pdb and /implib pointed at OUT_DIR, and copies the DLLs, configs, ROMs, saves and
assets beside the exe the first time (the build runs portable). Run from a shell with vcvars64 set up; point
tools/rush1/shots.py at the copy with RUSH2_EXE.
"""
import os, re, shutil, subprocess, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BUILD = os.path.join(REPO, 'build')


def ninja_unescape(s):
    return re.sub(r'\$(.)', r'\1', s)


def ninja_split(s):
    # Splits on spaces that aren't escaped as "$ ".
    parts, cur, i = [], '', 0
    while i < len(s):
        if s[i] == '$' and i + 1 < len(s):
            cur += s[i:i + 2]; i += 2; continue
        if s[i] == ' ':
            if cur: parts.append(ninja_unescape(cur))
            cur = ''
        else:
            cur += s[i]
        i += 1
    if cur: parts.append(ninja_unescape(cur))
    return parts


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    text = open(os.path.join(BUILD, 'build.ninja'), encoding='utf-8').read()
    m = re.search(r'^build Rush2Recompiled\.exe: \S+ (.*)\n((?:  .*\n)*)', text, re.M)
    inputs = ninja_split(m.group(1).split(' | ')[0])
    vars_ = dict(re.findall(r'^  (\w+) = (.*)$', m.group(2), re.M))
    rsp = ' '.join(inputs) + ' ' + vars_.get('LINK_PATH', '') + ' ' + vars_.get('LINK_LIBRARIES', '')
    open(os.path.join(BUILD, vars_['RSP_FILE']), 'w').write(rsp)

    cmd = subprocess.run(['ninja', '-t', 'commands', 'Rush2Recompiled.exe'], cwd=BUILD, capture_output=True,
                         text=True, check=True).stdout.strip().splitlines()[-1]
    cmd = cmd.replace('/out:Rush2Recompiled.exe', '/out:' + os.path.join(out, 'Rush2Recompiled.exe'))
    cmd = cmd.replace('/implib:Rush2Recompiled.lib', '/implib:' + os.path.join(out, 'Rush2Recompiled.lib'))
    cmd = cmd.replace('/pdb:Rush2Recompiled.pdb', '/pdb:' + os.path.join(out, 'Rush2Recompiled.pdb'))
    r = subprocess.run(cmd, cwd=BUILD, shell=True)
    if r.returncode != 0:
        sys.exit('link failed')

    if not os.path.exists(os.path.join(out, 'portable.txt')):
        for name in os.listdir(BUILD):
            src = os.path.join(BUILD, name)
            if name.endswith(('.dll', '.json', '.z64')) or name in ('portable.txt', 'recompcontrollerdb.txt'):
                shutil.copy2(src, out)
            elif name in ('saves', 'assets'):
                shutil.copytree(src, os.path.join(out, name), dirs_exist_ok=True)
    print('linked', os.path.join(out, 'Rush2Recompiled.exe'))


if __name__ == '__main__':
    main()
