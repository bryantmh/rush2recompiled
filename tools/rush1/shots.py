"""Launches Rush2Recompiled with an input script and saves screenshots of its window at given times (test aid).

Usage: python shots.py OUT_DIR "<input script>" t1 t2 ...    (times in seconds after launch)
Captures with PrintWindow (client area, full content), which works when other windows cover the game.
Set RUSH2_EXE to run another build (e.g. a copy linked into tmp/ while build/ is in use); it runs in its own folder.
Set RUSH2_WINDOW=WxH to resize the window's client area to W x H as soon as it appears (resolution tests).
"""
import ctypes, ctypes.wintypes as wt, os, subprocess, sys, time
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right > 200:
                found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def resize(hwnd, w, h):
    r = wt.RECT(0, 0, w, h)
    style = user32.GetWindowLongW(hwnd, -16)  # GWL_STYLE
    user32.AdjustWindowRect(ctypes.byref(r), style, False)
    # SWP_NOMOVE | SWP_NOZORDER
    user32.SetWindowPos(hwnd, None, 0, 0, r.right - r.left, r.bottom - r.top, 0x0002 | 0x0004)


def capture(hwnd, path):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 3)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                    ('biXPelsPerMeter', wt.LONG), ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                    ('biClrImportant', wt.DWORD)]
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(path)
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mdc); user32.ReleaseDC(hwnd, hdc)


if __name__ == '__main__':
    out, script, times = sys.argv[1], sys.argv[2], [float(t) for t in sys.argv[3:]]
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, 'run.log'), 'w')
    exe = os.path.abspath(os.environ.get('RUSH2_EXE', os.path.join(REPO, 'build', 'Rush2Recompiled.exe')))
    cwd = os.path.dirname(exe) if 'RUSH2_EXE' in os.environ else REPO
    p = subprocess.Popen([exe, '--input-script', script], cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
    start = time.time()
    size = os.environ.get('RUSH2_WINDOW')
    size = tuple(int(v) for v in size.lower().split('x')) if size else None
    try:
        for t in times:
            while time.time() - start < t:
                if p.poll() is not None:
                    print('game exited with', p.returncode); sys.exit(1)
                if size:
                    hwnd = find_window(p.pid)
                    if hwnd:
                        resize(hwnd, *size)
                        size = None
                time.sleep(0.05)
            hwnd = find_window(p.pid)
            if hwnd:
                path = os.path.join(out, 'shot_%05.1f.png' % t)
                capture(hwnd, path)
                print('saved', path)
            else:
                print('no window at', t)
    finally:
        p.terminate()
        try:
            p.wait(10)
        except Exception:
            p.kill()
