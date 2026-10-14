"""Launches the game, drives the recomp menu with posted keys and screenshots after each key (UI test aid).

Usage: python tools/menu_shot.py OUT_DIR KEY[,KEY...]
Keys: ESC UP DOWN LEFT RIGHT ENTER BACK TAB F15 F16 F17. ESC opens the menu on the General tab; F15 (the menu's Back)
moves focus to the tab bar, where RIGHT/LEFT pick a tab and ENTER opens it; arrows inside a page move focus, which
shows the focused row's description. Example, the Sound tab: ESC,DOWN,F15,RIGHT,RIGHT,ENTER
Set RUSH2_EXE to run another build (e.g. a copy linked into tmp/ while build/ is in use); it runs in its own folder.
Set RUSH2_LOG to a file path to keep the game's console output there.
Posted mouse clicks are ignored by the game, so everything goes through keys.
"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'rush1'))
import shots

VK = {'ESC': 0x1B, 'RIGHT': 0x27, 'LEFT': 0x25, 'UP': 0x26, 'DOWN': 0x28, 'ENTER': 0x0D, 'F17': 0x80, 'F16': 0x7F, 'F15': 0x7E, 'BACK': 0x08, 'TAB': 0x09}
out, keys = sys.argv[1], sys.argv[2].split(',')
os.makedirs(out, exist_ok=True)
exe = os.path.abspath(os.environ.get('RUSH2_EXE', os.path.join(shots.REPO, 'build', 'Rush2Recompiled.exe')))
log = open(os.environ['RUSH2_LOG'], 'w') if os.environ.get('RUSH2_LOG') else subprocess.DEVNULL
p = subprocess.Popen([exe], cwd=os.path.dirname(exe), stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(12)
    hwnd = shots.find_window(p.pid)
    for i, key in enumerate(keys):
        sc = shots.user32.MapVirtualKeyW(VK[key], 0) << 16
        if key in ('RIGHT', 'LEFT', 'UP', 'DOWN'):
            sc |= 1 << 24   # extended key, or SDL reads the numpad's
        shots.user32.PostMessageW(hwnd, 0x100, VK[key], sc | 1)
        time.sleep(0.1)
        shots.user32.PostMessageW(hwnd, 0x101, VK[key], sc | 0xC0000001)
        time.sleep(1.2)
        shots.capture(hwnd, os.path.join(out, '%02d_%s.png' % (i, key)))
finally:
    p.terminate()
