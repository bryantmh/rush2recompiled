"""Keeps one game session running and drives its menus with keys, for checking UI in a single launch (test aid).

Usage: python tools/ui_drive.py OUT_DIR ["<input script>"]
Launches Rush2Recompiled (RUSH2_EXE for another build, run in its own folder) and then follows commands appended to
OUT_DIR/cmd.txt, one per line, until `quit` (or the game exits):
    keys ESC RIGHT RIGHT ENTER    posts key presses to the window, 0.6 s apart (faster ones get dropped) (names: ESC ENTER SPACE TAB UP DOWN
                                  LEFT RIGHT BACK F1-F12, or a single letter or digit)
    click X Y                     left click at client pixel (X, Y), as the shots show it
    wheel X Y N                   mouse wheel at (X, Y), N notches (negative scrolls down)
    wait 1.5                      seconds
    shot NAME                     saves OUT_DIR/NAME.png (PrintWindow: works with other windows over the game)
    quit                          closes the game
Each finished command is echoed to OUT_DIR/done.txt, so a caller can tell when its shots exist. The game's output goes
to OUT_DIR/run.log. The window doesn't need focus: keys and mouse go through PostMessage.
"""
import ctypes, os, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'rush1'))
import shots  # noqa: E402  (find_window, capture)

user32 = ctypes.windll.user32
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEWHEEL = 0x0200, 0x0201, 0x0202, 0x020A
KEYS = {'ESC': 0x1B, 'ENTER': 0x0D, 'SPACE': 0x20, 'TAB': 0x09, 'BACK': 0x08,
        'LEFT': 0x25, 'UP': 0x26, 'RIGHT': 0x27, 'DOWN': 0x28}
KEYS.update({'F%d' % i: 0x6F + i for i in range(1, 13)})


def press(hwnd, name):
    vk = KEYS.get(name.upper(), ord(name.upper()) if len(name) == 1 else None)
    if vk is None:
        raise ValueError('unknown key ' + name)
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16))
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 1 | (scan << 16) | (0xC0 << 24))


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def click(hwnd, x, y):
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lparam(x, y))
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam(x, y))


def wheel(hwnd, x, y, notches):
    import ctypes.wintypes as wt
    pt = wt.POINT(x, y)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))  # WM_MOUSEWHEEL takes screen coordinates
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))
    for _ in range(abs(notches)):
        user32.PostMessageW(hwnd, WM_MOUSEWHEEL, ((120 if notches > 0 else -120) & 0xFFFF) << 16, lparam(pt.x, pt.y))
        time.sleep(0.05)


def main():
    out = sys.argv[1]
    script = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    cmd_path, done_path = os.path.join(out, 'cmd.txt'), os.path.join(out, 'done.txt')
    for p in (cmd_path, done_path):
        open(p, 'w').close()
    exe = os.path.abspath(os.environ.get('RUSH2_EXE', os.path.join(shots.REPO, 'build', 'Rush2Recompiled.exe')))
    cwd = os.path.dirname(exe) if 'RUSH2_EXE' in os.environ else shots.REPO
    args = [exe] + (['--input-script', script] if script else [])
    log = open(os.path.join(out, 'run.log'), 'w')
    game = subprocess.Popen(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
    handled = 0
    try:
        while game.poll() is None:
            lines = open(cmd_path).read().splitlines()
            if handled >= len(lines):
                time.sleep(0.1)
                continue
            line = lines[handled].strip()
            handled += 1
            if not line:
                continue
            word, _, rest = line.partition(' ')
            hwnd = shots.find_window(game.pid)
            if word == 'quit':
                break
            if word == 'wait':
                time.sleep(float(rest))
            elif word == 'keys' and hwnd:
                for k in rest.split():
                    press(hwnd, k)
                    time.sleep(0.6)
            elif word == 'click' and hwnd:
                x, y = (int(v) for v in rest.split())
                click(hwnd, x, y)
            elif word == 'wheel' and hwnd:
                x, y, n = (int(v) for v in rest.split())
                wheel(hwnd, x, y, n)
            elif word == 'shot' and hwnd:
                shots.capture(hwnd, os.path.join(out, rest + '.png'))
            with open(done_path, 'a') as f:
                f.write(line + '\n')
    finally:
        game.terminate()
        try:
            game.wait(10)
        except Exception:
            game.kill()


if __name__ == '__main__':
    main()
