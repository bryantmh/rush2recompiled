"""Times how long the game takes to close and shows what it is doing meanwhile: launches the exe, waits WAIT seconds,
asks the window to close (WM_CLOSE, then keys on the quit prompt if the game has started), prints how long the process
takes to exit, and at each of the --stacks times (seconds after the close) dumps every thread's call stack with cdb
(non-invasive attach) to OUT_DIR/stacks_<t>.txt, so a slow shutdown step shows up as the thread still sitting in it.

Usage: python tools/shutdown_time.py [--exe build/Rush2Recompiled.exe] [--script "<input script>"] [--wait 15]
                                     [--confirm LEFT,ENTER] [--stacks 1,3] [--out tmp/shutdown]
--script is passed to the game's --input-script (e.g. "8:START,11:A,13:A,15:A,17:A,19:A" races by ~22 s; use
--wait 30). --confirm are the keys that pick Quit on the prompt (posted clicks are ignored); OUT_DIR/key_<n>.png follows each; OUT_DIR/prompt.png shows the prompt as it opened.
"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'rush1'))
import shots

CDB = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'

ap = argparse.ArgumentParser()
ap.add_argument('--exe', default=os.path.join(shots.REPO, 'build', 'Rush2Recompiled.exe'))
ap.add_argument('--wait', type=float, default=15)
ap.add_argument('--script', default='')
ap.add_argument('--confirm', default='LEFT,ENTER')
ap.add_argument('--stacks', default='1,3')
ap.add_argument('--out', default=os.path.join(shots.REPO, 'tmp', 'shutdown'))
ap.add_argument('--timeout', type=float, default=60)
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
exe = os.path.abspath(args.exe)
p = subprocess.Popen([exe] + (['--input-script', args.script] if args.script else []), cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(args.wait)
hwnd = shots.find_window(p.pid)
shots.user32.PostMessageW(hwnd, 0x10, 0, 0)   # WM_CLOSE
t0 = time.monotonic()
time.sleep(1.0)
if p.poll() is None:   # in game: confirm the quit prompt
    shots.capture(hwnd, os.path.join(args.out, 'prompt.png'))
    VK = {'ENTER': 0x0D, 'SPACE': 0x20, 'LEFT': 0x25, 'RIGHT': 0x27, 'UP': 0x26, 'DOWN': 0x28, 'TAB': 0x09}
    keys = args.confirm.split(',')
    for i, key in enumerate(keys):
        sc = shots.user32.MapVirtualKeyW(VK[key], 0) << 16
        if key in ('LEFT', 'RIGHT', 'UP', 'DOWN'):
            sc |= 1 << 24   # extended key, or SDL reads the numpad's
        shots.user32.PostMessageW(hwnd, 0x100, VK[key], sc | 1)
        time.sleep(0.1)
        shots.user32.PostMessageW(hwnd, 0x101, VK[key], sc | 0xC0000001)
        if i == len(keys) - 1:
            break   # the last key quits: time from its release
        time.sleep(0.6)
        if p.poll() is None:
            shots.capture(hwnd, os.path.join(args.out, 'key_%d.png' % i))
    t0 = time.monotonic()
print('close sent')

pending = sorted(float(s) for s in args.stacks.split(',') if s)
while p.poll() is None and time.monotonic() - t0 < args.timeout:
    if pending and time.monotonic() - t0 >= pending[0]:
        t = pending.pop(0)
        path = os.path.join(args.out, 'stacks_%gs.txt' % t)
        out = subprocess.run([CDB, '-pv', '-p', str(p.pid), '-c', '.symfix+ ;.reload;~*kc 30;qd'],
                             capture_output=True, text=True, errors='replace').stdout
        open(path, 'w').write(out)
        print('%6.2f s  stacks -> %s' % (time.monotonic() - t0, path))
    time.sleep(0.02)
if p.poll() is None:
    print('still running after %g s, killing' % args.timeout)
    p.kill()
else:
    print('exited after %.2f s (code %d)' % (time.monotonic() - t0, p.returncode))
