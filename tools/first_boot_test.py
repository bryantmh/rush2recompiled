"""Tests the launcher's Locate ROM on a first boot (Windows): opens the file picker, cancels it, picks a file that isn't
a ROM (the error box), then the Rush 2 ROM, after which the setup screen (src/setup.cpp) picks the game rules and
closes from its other games step, and the launcher goes on to the build pass and the game. Never touches the player's
data: it runs as a silent simulation in its own empty data folder with the ROM import skipped (RUSH2_TEST_FIRST_BOOT,
docs/menus.md#launcher-on-a-first-boot). Screenshots and the game's log go to OUT_DIR.

Usage: python tools/first_boot_test.py OUT_DIR [ROM] [--rules original|enhanced]
ROM defaults to build/rush2.n64.us.z64; --rules (default enhanced) is the setup screen's choice, checked in the saved
settings.
Each dialog step prints whether it's owned by the game window and whether the game window is hung; exit code 1 on any
failure.
"""
import ctypes, ctypes.wintypes as wt, json, os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'rush1'))
import shots

u = shots.user32
u.GetWindow.restype = wt.HWND
args = sys.argv[1:]
rules = 'enhanced'
if '--rules' in args:
    i = args.index('--rules')
    rules = args[i + 1]
    del args[i:i + 2]
out = os.path.abspath(args[0])
rom = os.path.abspath(args[1] if len(args) > 1 else os.path.join(shots.REPO, 'build', 'rush2.n64.us.z64'))
data = os.path.join(out, 'data')
shutil.rmtree(out, ignore_errors=True)
os.makedirs(data)
not_rom = os.path.join(out, 'not_a_rom.txt')
open(not_rom, 'w').write('not a ROM')

WM_COMMAND, IDOK, IDCANCEL = 0x111, 1, 2
failures = []


def press_key(hwnd, vk, ext=0):
    sc = u.MapVirtualKeyW(vk, 0) << 16 | ext
    u.PostMessageW(hwnd, 0x100, vk, sc | 1)
    time.sleep(0.1)
    u.PostMessageW(hwnd, 0x101, vk, sc | 0xC0000001)
    time.sleep(1.2)


def press_enter(hwnd):
    press_key(hwnd, 0x0D)


def setup_section():
    try:
        return json.load(open(os.path.join(data, 'players.json'))).get('setup')
    except (OSError, ValueError):
        return None


def option(config, key):
    try:
        return json.load(open(os.path.join(data, config + '.json')))[key]
    except (OSError, ValueError, KeyError):
        return None


def title(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, b, 256)
    return b.value


def windows(pid, cls, parent=None):
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, b, 64)
        if b.value == cls and (parent is not None or (p.value == pid and u.IsWindowVisible(h))):
            found.append(h)
        return True
    if parent is None:
        u.EnumWindows(cb_t(cb), 0)
    else:
        u.EnumChildWindows(parent, cb_t(cb), 0)
    return found


def wait_dialog(pid, game, picker, what):
    # The picker is titled Open; the error box has the game's title.
    for _ in range(50):
        found = [h for h in windows(pid, '#32770') if (title(h) == 'Open') == picker]
        if found:
            d = found[0]
            owned, hung = u.GetWindow(d, 4) == game, bool(u.IsHungAppWindow(game))
            print('%s: "%s" owned by the game window=%s, game hung=%s' % (what, title(d), owned, hung), flush=True)
            if not owned or hung:
                failures.append(what)
            return d
        time.sleep(0.2)
    print('%s: didn\'t appear' % what, flush=True)
    failures.append(what)
    return None


def wait_gone(pid, d):
    for _ in range(25):
        if d not in windows(pid, '#32770'):
            return
        time.sleep(0.2)


env = dict(os.environ, RUSH2_SIM='1', RUSH2_SIM_TURBO='0', RUSH2_SIM_SHOW='1', RUSH2_TEST_FIRST_BOOT='1',
           RUSH2_DATA_DIR=data)
exe = os.path.join(shots.REPO, 'build', 'Rush2Recompiled.exe')
log = open(os.path.join(out, 'game.log'), 'w')
p = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(12)
    game = shots.find_window(p.pid)
    shots.capture(game, os.path.join(out, '0_launcher.png'))

    # Locate ROM is focused, or the first key press focuses it: press again if the picker didn't open.
    press_enter(game)
    if not [h for h in windows(p.pid, '#32770') if title(h) == 'Open']:
        press_enter(game)
    d = wait_dialog(p.pid, game, True, 'picker')
    if d:
        u.PostMessageW(d, WM_COMMAND, IDCANCEL, 0)
        wait_gone(p.pid, d)

    for path, expect_error in ((not_rom, True), (rom, False)):
        press_enter(game)
        d = wait_dialog(p.pid, game, True, 'picker')
        if not d:
            continue
        buf = ctypes.create_unicode_buffer(path)
        u.SendMessageW(windows(p.pid, 'Edit', d)[0], 0x0C, 0, buf)
        u.PostMessageW(d, WM_COMMAND, IDOK, 0)
        wait_gone(p.pid, d)
        if expect_error:
            e = wait_dialog(p.pid, game, False, 'error box')
            if e:
                u.PostMessageW(e, WM_COMMAND, IDOK, 0)
                wait_gone(p.pid, e)
        time.sleep(1.5)

    if not any(f.endswith('.z64') for f in os.listdir(data)):
        print('ROM not stored in the data folder', flush=True)
        failures.append('stored ROM')

    # The setup screen: Game Rules with Enhanced's card button focused (Original's is to its left); picking one shows
    # its settings, UP reaches Next, then the other games step opens with Done focused.
    time.sleep(1.5)
    shots.capture(game, os.path.join(out, '1_setup_rules.png'))
    s = setup_section()
    print('setup section before choosing:', s, flush=True)
    if not s or not s.get('pending'):
        failures.append('setup pending')
    if rules == 'original':
        press_key(game, 0x25, 1 << 24)   # left
    press_enter(game)
    time.sleep(1.0)
    shots.capture(game, os.path.join(out, '2_setup_picked.png'))
    if rules == 'original':
        press_key(game, 0x26, 1 << 24)   # up, to Next
        press_enter(game)
    else:
        press_key(game, 0x1B)            # Escape, which is Next on a first launch's Game Rules step
    time.sleep(1.0)
    shots.capture(game, os.path.join(out, '2_setup_games.png'))
    press_enter(game)                    # Done
    time.sleep(3.0)
    s = setup_section()
    print('setup section after Done:', s, flush=True)
    if not s or s.get('pending') or s.get('profile') != rules:
        failures.append('setup profile')
    enhanced = rules == 'enhanced'
    checks = (('general', 'torque_rebalance', enhanced), ('general', 'stunt_first_gear', enhanced),
              ('general', 'pause_menu_extras', enhanced), ('cheats', 'unlock_system', enhanced),
              ('games', 'wings', enhanced), ('games', 'stunt_scoring', 'Rush2049' if enhanced else 'Rush2'),
              ('games', 'rush2049_computer_cars', 'AllTracks' if enhanced else 'Rush2049Tracks'),
              ('games', 'calspeed_computer_cars', 'AllTracks' if enhanced else 'CalSpeedTracks'),
              ('graphics', 'ambient_occlusion', 'Off'))
    for config, key, want in checks:
        v = option(config, key)
        print('%s.%s = %s' % (config, key, v), flush=True)
        if v != want:
            failures.append('%s.%s' % (config, key))
    shots.capture(game, os.path.join(out, '3_after.png'))
finally:
    p.terminate()

print('FAILED: ' + ', '.join(failures) if failures else 'OK (check 1_setup_rules.png and 2_setup_games.png show the setup '
      'screen, and 3_after.png the game or the build progress screen, never Start Game)')
sys.exit(1 if failures else 0)
