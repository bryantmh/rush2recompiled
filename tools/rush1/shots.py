"""Launches Rush2Recompiled with an input script and saves screenshots of its window at given times (test aid).

Usage: python shots.py OUT_DIR "<input script>" t1 t2 ...    (times in seconds after launch)
Captures with PrintWindow (client area, full content), which works when other windows cover the game.
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
    exe = os.path.join(REPO, 'build', 'Rush2Recompiled.exe')
    p = subprocess.Popen([exe, '--input-script', script], cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
    start = time.time()
    try:
        for t in times:
            while time.time() - start < t:
                if p.poll() is not None:
                    print('game exited with', p.returncode); sys.exit(1)
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
