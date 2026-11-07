"""Starts test runs of the game on a hidden desktop (test aid; the tools that launch the game all go through it).

popen() is subprocess.Popen with the child placed on a Win32 desktop of its own, "Rush2Test" (CreateDesktopW +
STARTUPINFO.lpDesktop, as TESConversion's tools/live/hidden_launch.py does for Skyrim): the game renders, ticks and
plays sound normally there, but its window never appears on the user's screen, takes focus or reads their keyboard
and mouse, so a test run doesn't get in the way of whoever is using the computer. It returns a real Popen (poll,
wait, terminate, PIPE output all work).

The run's windows are found with find_window(pid) / windows() (EnumDesktopWindows over that desktop as well as the
normal one), captured with PrintWindow (rush1/shots.py capture) and driven with PostMessage, as before.

Set RUSH2_VISIBLE=1 to start runs on the normal desktop instead, to watch one.
See docs/menus.md#hidden-desktop.
"""
import ctypes
import os
import subprocess
import threading
from ctypes import wintypes as wt

DESKTOP = 'Rush2Test'

_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_u32 = ctypes.WinDLL('user32', use_last_error=True)

GENERIC_ALL = 0x10000000
CREATE_UNICODE_ENVIRONMENT = 0x400
EXTENDED_STARTUPINFO_PRESENT = 0x80000
PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x20002


class _StartupInfo(ctypes.Structure):
    _fields_ = (('cb', wt.DWORD), ('lpReserved', wt.LPWSTR), ('lpDesktop', wt.LPWSTR), ('lpTitle', wt.LPWSTR),
                ('dwX', wt.DWORD), ('dwY', wt.DWORD), ('dwXSize', wt.DWORD), ('dwYSize', wt.DWORD),
                ('dwXCountChars', wt.DWORD), ('dwYCountChars', wt.DWORD), ('dwFillAttribute', wt.DWORD),
                ('dwFlags', wt.DWORD), ('wShowWindow', wt.WORD), ('cbReserved2', wt.WORD),
                ('lpReserved2', ctypes.c_void_p), ('hStdInput', wt.HANDLE), ('hStdOutput', wt.HANDLE),
                ('hStdError', wt.HANDLE))


class _StartupInfoEx(ctypes.Structure):
    _fields_ = (('StartupInfo', _StartupInfo), ('lpAttributeList', ctypes.c_void_p))


class _ProcessInformation(ctypes.Structure):
    _fields_ = (('hProcess', wt.HANDLE), ('hThread', wt.HANDLE), ('dwProcessId', wt.DWORD),
                ('dwThreadId', wt.DWORD))


_u32.CreateDesktopW.restype = wt.HANDLE
_u32.CreateDesktopW.argtypes = (wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p, wt.DWORD, wt.DWORD, ctypes.c_void_p)
_ENUM_PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
_u32.EnumDesktopWindows.argtypes = (wt.HANDLE, _ENUM_PROC, wt.LPARAM)
_u32.EnumWindows.argtypes = (_ENUM_PROC, wt.LPARAM)
_u32.EnumChildWindows.argtypes = (wt.HWND, _ENUM_PROC, wt.LPARAM)
_u32.GetWindowThreadProcessId.argtypes = (wt.HWND, ctypes.POINTER(wt.DWORD))
_u32.GetClassNameW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
_u32.IsWindowVisible.argtypes = (wt.HWND,)
_u32.GetClientRect.argtypes = (wt.HWND, ctypes.POINTER(wt.RECT))
_k32.CreateProcessW.argtypes = (wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL, wt.DWORD,
                                ctypes.c_void_p, wt.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p)
_k32.InitializeProcThreadAttributeList.argtypes = (ctypes.c_void_p, wt.DWORD, wt.DWORD,
                                                   ctypes.POINTER(ctypes.c_size_t))
_k32.UpdateProcThreadAttribute.argtypes = (ctypes.c_void_p, wt.DWORD, ctypes.c_size_t, ctypes.c_void_p,
                                           ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p)
_k32.DeleteProcThreadAttributeList.argtypes = (ctypes.c_void_p,)

_lock = threading.Lock()
_desk = None   # kept open for this process's life: closing it invalidates the run's window handles here


def visible():
    """Whether runs go on the normal desktop (RUSH2_VISIBLE=1)."""
    return os.environ.get('RUSH2_VISIBLE', '') not in ('', '0')


def _desktop():
    """The hidden desktop's handle, creating the desktop (or opening it when it exists) the first time."""
    global _desk
    if _desk is None:
        desk = _u32.CreateDesktopW(DESKTOP, None, None, 0, GENERIC_ALL, None)
        if not desk:
            raise ctypes.WinError(ctypes.get_last_error())
        _desk = desk
    return _desk


def _create_process(executable, args, _proc_attrs, _thread_attrs, inherit, flags, env, cwd, startupinfo):
    """_winapi.CreateProcess with lpDesktop set to the hidden desktop; the same arguments and results."""
    info = _StartupInfoEx()
    si = info.StartupInfo
    si.cb = ctypes.sizeof(_StartupInfoEx)
    si.lpDesktop = 'WinSta0\\' + DESKTOP
    attrs = None
    if startupinfo is not None:
        si.dwFlags = startupinfo.dwFlags
        si.wShowWindow = startupinfo.wShowWindow
        si.hStdInput = int(startupinfo.hStdInput or 0) or None
        si.hStdOutput = int(startupinfo.hStdOutput or 0) or None
        si.hStdError = int(startupinfo.hStdError or 0) or None
        handles = [int(h) for h in ((startupinfo.lpAttributeList or {}).get('handle_list') or [])]
        if handles:   # Popen's close_fds: only these handles are inherited
            size = ctypes.c_size_t()
            _k32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
            attrs = ctypes.create_string_buffer(size.value)
            if not _k32.InitializeProcThreadAttributeList(attrs, 1, 0, ctypes.byref(size)):
                raise ctypes.WinError(ctypes.get_last_error())
            harr = (wt.HANDLE * len(handles))(*handles)
            if not _k32.UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, harr,
                                                  ctypes.sizeof(harr), None, None):
                raise ctypes.WinError(ctypes.get_last_error())
            info.lpAttributeList = ctypes.cast(attrs, ctypes.c_void_p)
            flags |= EXTENDED_STARTUPINFO_PRESENT
    block = None
    if env is not None:
        text = ''.join('%s=%s\0' % kv for kv in sorted(env.items(), key=lambda kv: kv[0].upper())) + '\0'
        block = ctypes.create_unicode_buffer(text, len(text))
        flags |= CREATE_UNICODE_ENVIRONMENT
    pi = _ProcessInformation()
    cmd = ctypes.create_unicode_buffer(args)
    try:
        ok = _k32.CreateProcessW(executable, cmd, None, None, bool(inherit), flags, block, cwd,
                                 ctypes.byref(info), ctypes.byref(pi))
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        if attrs is not None:
            _k32.DeleteProcThreadAttributeList(attrs)
    return pi.hProcess, pi.hThread, pi.dwProcessId, pi.dwThreadId


def popen(args, **kwargs):
    """subprocess.Popen(args, **kwargs), started on the hidden desktop unless RUSH2_VISIBLE=1."""
    if visible():
        return subprocess.Popen(args, **kwargs)
    _desktop()
    winapi = subprocess._winapi
    with _lock:
        real = winapi.CreateProcess
        winapi.CreateProcess = _create_process
        try:
            return subprocess.Popen(args, **kwargs)
        finally:
            winapi.CreateProcess = real


def windows(pid=None, window_class=None, visible_only=True):
    """Top-level windows of process `pid` (any when None) of class `window_class` (any when None), on the hidden
    desktop and the normal one."""
    found = []

    def visit(hwnd, _):
        if pid is not None:
            p = wt.DWORD()
            _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value != pid:
                return True
        if visible_only and not _u32.IsWindowVisible(hwnd):
            return True
        if window_class is not None:
            name = ctypes.create_unicode_buffer(256)
            _u32.GetClassNameW(hwnd, name, 256)
            if name.value != window_class:
                return True
        found.append(hwnd)
        return True

    cb = _ENUM_PROC(visit)
    try:
        _u32.EnumDesktopWindows(_desktop(), cb, 0)
    except OSError:
        pass
    _u32.EnumWindows(cb, 0)
    return found


def find_window(pid):
    """The game window of process `pid` (its first visible top-level window wider than 200 px), or None."""
    for hwnd in windows(pid):
        r = wt.RECT()
        _u32.GetClientRect(hwnd, ctypes.byref(r))
        if r.right > 200:
            return hwnd
    return None
