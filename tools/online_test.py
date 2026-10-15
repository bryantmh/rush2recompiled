#!/usr/bin/env python3
"""Runs several game instances on this machine for an online test and screenshots each window at given times (test aid,
docs/online.md#test-aids).

    python tools/online_test.py OUT_DIR --times 25,35 \
        --instance "host|RUSH2_ONLINE_TEST=host:2 RUSH2_ONLINE_TRACK=0|" \
        --instance "join|RUSH2_ONLINE_TEST=join:127.0.0.1|30:A:20"

Each --instance is "name|environment variables (space separated NAME=VALUE)|input script" (the script as
--input-script takes it, may be empty). Instances start in order, --stagger seconds apart (default 3), all from the
same exe (RUSH2_EXE, default build/Rush2Recompiled.exe) in its folder. Times count from the first launch. Writes
OUT_DIR/<name>.log (the game's console) and OUT_DIR/<name>_<time>.png, then closes the instances it started (by PID).
--hold "name:START-END:KEY" holds a key on that instance's window from START to END seconds (posted key messages,
repeated like a held key, so they reach the game's keyboard input without focus): the real keyboard path the
--input-script presses skip (port assignment, bindings, control profile). Keys: UP DOWN LEFT RIGHT ENTER SPACE E Q
ESC F15 BACK TAB (ESC opens the recomp menu, F15 moves its focus to the tab bar; tools/menu_shot.py).
--key "name:T:KEY[,KEY...]" taps the keys on that instance's window at T seconds (0.7 s apart).
--close "name:T" asks that instance's window to close at T seconds (WM_CLOSE; in a race the game asks first, so follow
with --key "name:T+1:LEFT,ENTER") and prints how long it takes to exit, for a slow or crashing close.
"""
import argparse, os, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush1'))
import shots  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--instance', action='append', required=True)
ap.add_argument('--times', required=True)
ap.add_argument('--stagger', type=float, default=3.0)
ap.add_argument('--hold', action='append', default=[])
ap.add_argument('--key', action='append', default=[])
ap.add_argument('--close', action='append', default=[])
ap.add_argument('--log-focus', action='store_true', help='print whenever the foreground window changes owner')
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
exe = os.path.abspath(os.environ.get('RUSH2_EXE', os.path.join(shots.REPO, 'build', 'Rush2Recompiled.exe')))
times = [float(t) for t in args.times.split(',')]
procs = []
start = time.time()
try:
    for i, spec in enumerate(args.instance):
        name, env_text, script = (spec.split('|') + ['', ''])[:3]
        env = dict(os.environ)
        for pair in env_text.split():
            k, _, v = pair.partition('=')
            env[k] = v
        cmd = [exe] + (['--input-script', script] if script else [])
        log = open(os.path.join(args.out, name + '.log'), 'w')
        procs.append((name, subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log,
                                             stderr=subprocess.STDOUT)))
        print('started', name, 'pid', procs[-1][1].pid, 'at %.1f' % (time.time() - start))
        if i + 1 < len(args.instance):
            time.sleep(args.stagger)
    VK = {'UP': 0x26, 'DOWN': 0x28, 'LEFT': 0x25, 'RIGHT': 0x27, 'ENTER': 0x0D, 'SPACE': 0x20, 'E': 0x45, 'Q': 0x51,
          'ESC': 0x1B, 'F15': 0x7E, 'BACK': 0x08, 'TAB': 0x09}
    holds = []
    for h in args.hold:
        name, span, key = h.split(':')
        a, b = (float(v) for v in span.split('-'))
        holds.append([name, a, b, VK[key], False])

    def post_holds():
        now = time.time() - start
        for h in holds:
            name, a, b, vk, down = h
            p = dict(procs).get(name)
            hwnd = shots.find_window(p.pid) if p is not None and p.poll() is None else None
            if hwnd is None:
                continue
            sc = shots.user32.MapVirtualKeyW(vk, 0) << 16
            if vk in (0x25, 0x26, 0x27, 0x28):
                sc |= 1 << 24   # extended key, or SDL reads the numpad's
            if a <= now < b:
                shots.user32.PostMessageW(hwnd, 0x100, vk, sc | (0x40000001 if down else 1))
                h[4] = True
            elif down and now >= b:
                shots.user32.PostMessageW(hwnd, 0x101, vk, sc | 0xC0000001)
                h[4] = False

    # One-off events: (time, name, key or None for close).
    events = []
    for k in args.key:
        name, t, keys = k.split(':')
        for i, key in enumerate(keys.split(',')):
            events.append((float(t) + i * 0.7, name, key))
    for c in args.close:
        name, t = c.split(':')
        events.append((float(t), name, None))
    events.sort(key=lambda e: e[0])
    closing = {}  # name -> time the close was sent

    def post_events():
        now = time.time() - start
        while events and events[0][0] <= now:
            _, name, key = events.pop(0)
            p = dict(procs).get(name)
            hwnd = shots.find_window(p.pid) if p is not None and p.poll() is None else None
            if hwnd is None:
                continue
            if key is None:
                shots.user32.PostMessageW(hwnd, 0x10, 0, 0)  # WM_CLOSE
                closing[name] = time.time()
                print('%.1f s: close sent to %s' % (now, name))
                continue
            vk = VK[key]
            sc = shots.user32.MapVirtualKeyW(vk, 0) << 16
            if vk in (0x25, 0x26, 0x27, 0x28):
                sc |= 1 << 24   # extended key, or SDL reads the numpad's
            shots.user32.PostMessageW(hwnd, 0x100, vk, sc | 1)
            time.sleep(0.1)
            shots.user32.PostMessageW(hwnd, 0x101, vk, sc | 0xC0000001)
            if name in closing:
                closing[name] = time.time()  # A key on the quit prompt: time the exit from it.
        for name, p in procs:
            if name in closing and p.poll() is not None:
                print('%s exited %.2f s after its close (code %d)' % (name, time.time() - closing.pop(name),
                                                                       p.returncode))

    last_focus = None

    def log_focus():
        global last_focus
        if not args.log_focus:
            return
        pid = shots.wt.DWORD()
        shots.user32.GetWindowThreadProcessId(shots.user32.GetForegroundWindow(), shots.ctypes.byref(pid))
        owner = next((name for name, p in procs if p.pid == pid.value), 'other (pid %d)' % pid.value)
        if owner != last_focus:
            print('%.1f s: focus on %s' % (time.time() - start, owner))
            last_focus = owner

    for t in times:
        while time.time() - start < t:
            post_holds()
            post_events()
            log_focus()
            time.sleep(0.05)
        for name, p in procs:
            if p.poll() is not None:
                continue
            hwnd = shots.find_window(p.pid)
            if hwnd:
                path = os.path.join(args.out, '%s_%05.1f.png' % (name, t))
                shots.capture(hwnd, path)
                print('saved', path)
            else:
                print(name, 'has no window at', t)
    # Closes still running: wait for them to exit (or report that they didn't).
    deadline = time.time() + 30
    while (closing or events) and time.time() < deadline:
        post_events()
        time.sleep(0.05)
    for name in closing:
        print(name, 'still running 30 s after its close')
finally:
    for name, p in procs:
        p.terminate()
    for name, p in procs:
        try:
            p.wait(10)
        except Exception:
            p.kill()
