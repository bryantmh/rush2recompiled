"""Builds Rush2Recompiled.exe. The one build entry point for every session (tools/build.cmd calls it too).

Usage: python tools/build.py [--regen] [--target T] [--log PATH]

  --regen      rerun N64Recomp even if its inputs look unchanged
  --target T   build one ninja target (e.g. CMakeFiles/Rush2Recompiled.dir/src/x.cpp.obj) instead of everything
  --log PATH   also write the output here (default tmp/build.log)

Several sessions share the tree and build/. This script keeps them out of each other's way:
- One build at a time: an OS lock on tmp/build.lock (released by the OS if the holder dies). A second caller waits
  for the running build, then runs ninja itself. If the other build already covered its edits, ninja finds nothing
  to do and returns in seconds; otherwise only what changed since is rebuilt. Never start a second build by hand.
- No redundant work: ninja decides what is stale. N64Recomp reruns only when us.toml, the syms file, the ROM or
  N64Recomp itself is newer than build/recomp_regen.stamp (or --regen). RecompiledFuncs files whose text didn't
  change keep their old timestamps, so a regen recompiles only the files it really changed; if the set of files
  changed, CMake is reconfigured so its glob picks them up.
- No waiting on a running game: if the exe has to be relinked while a game (the user's or a test run) holds it,
  the exe is renamed to Rush2Recompiled.<time>.old and the link goes ahead. Windows allows renaming a running exe;
  the game keeps running and the next launch gets the new build. Nothing is killed. .old files that are no longer
  in use are deleted at the start of each build.
Exit code is the build's.
"""
import argparse, glob, hashlib, json, os, re, shutil, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BUILD = os.path.join(REPO, 'build')
TMP = os.path.join(REPO, 'tmp')
EXE = os.path.join(BUILD, 'Rush2Recompiled.exe' if os.name == 'nt' else 'Rush2Recompiled')
FUNCS = os.path.join(REPO, 'RecompiledFuncs')
N64RECOMP = os.path.join(REPO, '.tools', 'N64Recomp.exe' if os.name == 'nt' else 'N64Recomp')
REGEN_INPUTS = [os.path.join(REPO, p) for p in ('us.toml', 'syms/rush2.us.syms.toml', 'rush2.us.recomp.z64')]
REGEN_STAMP = os.path.join(BUILD, 'recomp_regen.stamp')
LOCK = os.path.join(TMP, 'build.lock')
LOCK_INFO = os.path.join(TMP, 'build.lock.info')
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
CMAKE_DIRS = [r'C:\Program Files\CMake\bin', r'C:\Program Files (x86)\Microsoft Visual Studio\Installer']
# lld-link and link.exe wording for "can't write the exe".
LINK_LOCKED = re.compile(r"failed to write output '[^']*Rush2Recompiled|LNK1104: cannot open file '[^']*Rush2Recompiled",
                         re.I)


LOG = None  # the open log file once the build starts


def say(msg):
    print(f'[build] {msg}', flush=True)
    if LOG:
        LOG.write(f'[build] {msg}\n')
        LOG.flush()


def foreign_ninjas():
    """ninja.exe processes running now. Called while holding the lock, so any found are builds started without this
    script (a raw `cmake --build`); running beside one damages build/.ninja_log and forces a full rebuild."""
    if os.name != 'nt':
        return []
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ninja.exe', '/FO', 'CSV', '/NH'], capture_output=True,
                         text=True, errors='replace').stdout
    return [line.split('","')[1] for line in out.splitlines() if line.startswith('"ninja.exe"')]


def wait_for_foreign_ninjas():
    t0, last = time.time(), -1
    while pids := foreign_ninjas():
        waited = int(time.time() - t0)
        if waited // 30 != last:
            last = waited // 30
            say(f'a build started without tools/build.py is running (ninja pid {", ".join(pids)}); waiting for it '
                f'({waited} s so far)')
        time.sleep(2)


# ---- lock -------------------------------------------------------------------------------------------------------

def try_lock(f):
    try:
        if os.name == 'nt':
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def acquire_lock(args):
    os.makedirs(TMP, exist_ok=True)
    f = open(LOCK, 'a+')
    t0 = time.time()
    last = -1
    while not try_lock(f):
        waited = int(time.time() - t0)
        if waited // 30 != last:
            last = waited // 30
            try:
                info = json.load(open(LOCK_INFO))
                who = (f"pid {info.get('pid')}, {info.get('what')}, started "
                       f"{int(time.time() - info.get('start', time.time()))} s ago")
            except (OSError, ValueError):
                who = 'unknown holder'
            say(f'another build is running ({who}); waiting for it, then building only what it left stale '
                f'({waited} s so far)')
        time.sleep(2)
    with open(LOCK_INFO, 'w') as g:
        json.dump({'pid': os.getpid(), 'start': time.time(), 'what': args.target or 'full build',
                   'cwd': os.getcwd()}, g)
    return f


# ---- environment ------------------------------------------------------------------------------------------------

def build_env():
    env = dict(os.environ)
    if os.name != 'nt':
        return env
    env['PATH'] = os.pathsep.join(CMAKE_DIRS + [env.get('PATH', '')])
    if 'VCToolsInstallDir' in env:
        return env
    out = subprocess.run(f'cmd /s /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, env=env,
                         errors='replace')
    if out.returncode != 0:
        say('vcvars64.bat failed:\n' + out.stdout + out.stderr)
        sys.exit(1)
    for line in out.stdout.splitlines():
        k, sep, v = line.partition('=')
        if sep and k:
            env[k] = v
    return env


def run(cmd, env, log, capture=False):
    """Runs cmd, echoing its output to stdout and the log. Returns (exit code, output text)."""
    p = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines = []
    for raw in p.stdout:
        line = raw.decode('utf-8', errors='replace')
        lines.append(line)
        if not capture:
            sys.stdout.write(line)
            sys.stdout.flush()
            log.write(line)
            log.flush()
    return p.wait(), ''.join(lines)


# ---- N64Recomp regen --------------------------------------------------------------------------------------------

def mtime(p):
    try:
        return os.stat(p).st_mtime_ns
    except OSError:
        return None


def regen_needed():
    stamp = mtime(REGEN_STAMP)
    if stamp is None or not os.path.isdir(FUNCS):
        return True
    return any((mtime(p) or 0) > stamp for p in REGEN_INPUTS + [N64RECOMP])


def func_files():
    return {os.path.basename(p): p for p in glob.glob(os.path.join(FUNCS, '*'))
            if os.path.isfile(p)}


def digest(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.digest()


def regen(env, log):
    before = {n: (digest(p), os.stat(p)) for n, p in func_files().items()}
    say('running N64Recomp on us.toml')
    t = time.time()
    code, _ = run([N64RECOMP, 'us.toml'], env, log)
    if code != 0:
        say('N64Recomp failed')
        return code, False
    after = func_files()
    changed = 0
    for n, p in after.items():
        old = before.get(n)
        if old and digest(p) == old[0]:
            os.utime(p, ns=(old[1].st_atime_ns, old[1].st_mtime_ns))  # unchanged text: keep it built
        else:
            changed += 1
    say(f'N64Recomp done in {time.time() - t:.0f} s; {changed} of {len(after)} RecompiledFuncs files changed')
    os.makedirs(BUILD, exist_ok=True)
    with open(REGEN_STAMP, 'w') as f:
        f.write(time.ctime())
    return 0, set(after) != set(before)


# ---- running exe ------------------------------------------------------------------------------------------------

def exe_in_use():
    if not os.path.exists(EXE):
        return False
    try:
        with open(EXE, 'r+b'):
            return False
    except PermissionError:
        return True
    except OSError:
        return True


def move_exe_aside():
    old = os.path.join(BUILD, f'Rush2Recompiled.{time.strftime("%Y%m%d-%H%M%S")}.old')
    os.rename(EXE, old)
    say(f'the exe is in use by a running game; renamed it to {os.path.basename(old)} (the game keeps running, '
        f'the next launch gets the new build)')


def clean_old_exes():
    for p in glob.glob(os.path.join(BUILD, 'Rush2Recompiled*.old')):
        try:
            os.remove(p)
        except OSError:
            pass  # still running


def link_pending(env, args):
    """Ninja dry run: will this build relink the exe?"""
    cmd = ['cmake', '--build', 'build'] + (['--target', args.target] if args.target else []) + ['--', '-n']
    _, out = run(cmd, env, None, capture=True)
    return re.search(r'Linking \S+ executable Rush2Recompiled', out) is not None


# ---- main -------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--regen', action='store_true')
    ap.add_argument('--target')
    ap.add_argument('--log', default=os.path.join(TMP, 'build.log'))
    args = ap.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')

    lock = acquire_lock(args)
    t0 = time.time()
    env = build_env()
    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    with open(args.log, 'w', encoding='utf-8', errors='replace') as log:
        global LOG
        LOG = log
        wait_for_foreign_ninjas()
        clean_old_exes()
        if args.regen or regen_needed():
            code, files_changed = regen(env, log)
            if code != 0:
                return code
            if files_changed:
                say('RecompiledFuncs file set changed; reconfiguring CMake')
                code, _ = run(['cmake', '-S', '.', '-B', 'build'], env, log)
                if code != 0:
                    return code
        if exe_in_use() and link_pending(env, args):
            move_exe_aside()
        cmd = ['cmake', '--build', 'build'] + (['--target', args.target] if args.target else [])
        code, out = run(cmd, env, log)
        if code != 0 and LINK_LOCKED.search(out) and exe_in_use():
            # A game started between the check and the link.
            move_exe_aside()
            code, out = run(cmd, env, log)
        if code == 0 and 'ninja: no work to do' in out:
            say(f'already up to date (another build covered these edits); {time.time() - t0:.0f} s')
        elif code == 0:
            say(f'build succeeded in {time.time() - t0:.0f} s')
        else:
            say(f'build FAILED (exit {code}); log: {os.path.relpath(args.log, REPO)}')
    lock.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
