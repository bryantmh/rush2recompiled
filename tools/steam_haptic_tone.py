"""Plays a tone on a Steam Controller (2026) actuator, the way src/steam_haptics.cpp does (haptics tuning aid).

Usage: python tools/steam_haptic_tone.py [--actuator N] [--hz F] [--gain DB] [--lfo HZ] [--depth PCT] [--ms MS]
Sends output report 0x83 (LFO tone) to the first connected Steam Controller (wired 28DE:1302, Bluetooth 1303 or the
puck 1304/1305), holds it for --ms milliseconds, then silences it. Actuators: 0 left trackpad, 1 right trackpad,
3 left grip, 4 right grip (default 3). Gain is in dB, 0 = full (positive clips). Windows only. Prints each HID
interface it tries, so it also shows which one takes the reports.
"""
import argparse
import ctypes
import time
from ctypes import wintypes

hid = ctypes.WinDLL('hid')
setupapi = ctypes.WinDLL('setupapi')
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wintypes.DWORD), ('Data2', wintypes.WORD), ('Data3', wintypes.WORD), ('Data4', ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('InterfaceClassGuid', GUID), ('Flags', wintypes.DWORD),
                ('Reserved', ctypes.c_void_p)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [('Usage', wintypes.USHORT), ('UsagePage', wintypes.USHORT), ('InputReportByteLength', wintypes.USHORT),
                ('OutputReportByteLength', wintypes.USHORT), ('FeatureReportByteLength', wintypes.USHORT),
                ('Reserved', wintypes.USHORT * 17), ('rest', wintypes.USHORT * 10)]


def hid_paths():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x12)  # PRESENT | DEVICEINTERFACE
    i = 0
    while True:
        data = SP_DEVICE_INTERFACE_DATA()
        data.cbSize = ctypes.sizeof(data)
        if not setupapi.SetupDiEnumDeviceInterfaces(ctypes.c_void_p(info), None, ctypes.byref(guid), i, ctypes.byref(data)):
            break
        size = wintypes.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(info), ctypes.byref(data), None, 0, ctypes.byref(size), None)
        buf = ctypes.create_string_buffer(size.value)
        ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = 8  # cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W on x64
        if setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(info), ctypes.byref(data), buf, size, None, None):
            yield ctypes.wstring_at(ctypes.addressof(buf) + 4)
        i += 1


def open_device(path):
    kernel32.CreateFileW.restype = wintypes.HANDLE
    handle = kernel32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)  # read/write, share read/write, OPEN_EXISTING
    if handle == wintypes.HANDLE(-1).value:
        return None, 0
    data = ctypes.c_void_p()
    caps = HIDP_CAPS()
    if hid.HidD_GetPreparsedData(wintypes.HANDLE(handle), ctypes.byref(data)):
        hid.HidP_GetCaps(data, ctypes.byref(caps))
        hid.HidD_FreePreparsedData(data)
    return handle, caps.OutputReportByteLength


def tone_report(size, actuator, gain, hz, ms, lfo, depth):
    report = bytearray(size)
    report[0:10] = bytes([0x83, actuator, gain & 0xFF, hz & 0xFF, hz >> 8, ms & 0xFF, (ms >> 8) & 0xFF,
                          lfo & 0xFF, lfo >> 8, depth])
    return bytes(report)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--actuator', type=int, default=3)
    p.add_argument('--hz', type=int, default=170)
    p.add_argument('--gain', type=int, default=-6)
    p.add_argument('--lfo', type=int, default=0)
    p.add_argument('--depth', type=int, default=0)
    p.add_argument('--ms', type=int, default=600)
    a = p.parse_args()
    for path in hid_paths():
        low = path.lower()
        if 'vid_28de' not in low or not any('pid_%04x' % pid in low for pid in (0x1302, 0x1303, 0x1304, 0x1305)):
            continue
        handle, size = open_device(path)
        print(path, 'open' if handle else 'not opened', 'output report bytes', size)
        if not handle or size < 10:
            if handle:
                kernel32.CloseHandle(wintypes.HANDLE(handle))
            continue
        written = wintypes.DWORD()
        ok = kernel32.WriteFile(wintypes.HANDLE(handle), tone_report(size, a.actuator, a.gain, a.hz, 0x7FFF, a.lfo, a.depth),
                                size, ctypes.byref(written), None)
        print('  tone written:', bool(ok), 'error', ctypes.get_last_error() if not ok else 0)
        time.sleep(a.ms / 1000.0)
        kernel32.WriteFile(wintypes.HANDLE(handle), tone_report(size, a.actuator, -128, 0, 0x8000, 0, 0), size,
                           ctypes.byref(written), None)
        kernel32.CloseHandle(wintypes.HANDLE(handle))
        if ok:
            return
    print('No Steam Controller took the report')


if __name__ == '__main__':
    main()
