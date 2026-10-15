#!/usr/bin/env python3
"""Reads a running game's RDRAM (read-only), to look at a live game's state instead of reproducing it.

    python tools/live_ram.py PID ADDR[:LEN] ...        hex dump (N64 byte order) of each range
    python tools/live_ram.py PID --w ADDR ...          32-bit words  (--h halfwords, --b bytes, signed)

ADDR is an N64 address (0x80xxxxxx). RDRAM is the process's 4 GB reservation (librecomp recomp.cpp, VirtualAlloc)
whose first part (512 MB in this build) is read-write and the rest no-access; it holds 32-bit words in host order, so
byte N64 address A is at A ^ 3.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
RDRAM_SIZE = 16 * 1024 * 1024  # Read: 0x80000000-0x80FFFFFF.
ALLOCATION_SIZE = 1 << 32


def find_rdram(h):
    addr = 0
    mbi = MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        # The RDRAM block: a read-write region at the start of its allocation, the rest of the 4 GB no-access.
        if (mbi.AllocationBase == mbi.BaseAddress and mbi.RegionSize >= RDRAM_SIZE and mbi.Protect == 0x04 and
                mbi.State == 0x1000):
            nxt = MBI()
            if k32.VirtualQueryEx(h, ctypes.c_void_p(base + mbi.RegionSize), ctypes.byref(nxt), ctypes.sizeof(nxt)) \
                    and nxt.AllocationBase == mbi.AllocationBase and nxt.Protect == 0x01 and \
                    mbi.RegionSize + nxt.RegionSize == ALLOCATION_SIZE:
                return base
        addr = base + mbi.RegionSize
        if addr >= 1 << 47:
            break
    return None


def main():
    pid = int(sys.argv[1])
    h = k32.OpenProcess(0x0410, False, pid)  # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if not h:
        sys.exit(f'cannot open process {pid}')
    base = find_rdram(h)
    if base is None:
        sys.exit('RDRAM not found')
    buf = ctypes.create_string_buffer(RDRAM_SIZE)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, RDRAM_SIZE, ctypes.byref(got)):
        sys.exit('read failed')
    ram = buf.raw

    def byte(a):
        return ram[((a & 0x7FFFFF) ^ 3)]

    mode = None
    for arg in sys.argv[2:]:
        if arg in ('--w', '--h', '--b'):
            mode = arg
            continue
        a, _, n = arg.partition(':')
        a = int(a, 16)
        if mode == '--w':
            v = struct.unpack('>i', bytes(byte(a + i) for i in range(4)))[0]
            print(f'{a:08X} {v & 0xFFFFFFFF:08X} ({v})')
        elif mode == '--h':
            v = struct.unpack('>h', bytes(byte(a + i) for i in range(2)))[0]
            print(f'{a:08X} {v & 0xFFFF:04X} ({v})')
        elif mode == '--b':
            v = struct.unpack('>b', bytes([byte(a)]))[0]
            print(f'{a:08X} {v & 0xFF:02X} ({v})')
        else:
            n = int(n, 16) if n else 0x40
            for row in range(0, n, 16):
                bs = bytes(byte(a + row + i) for i in range(min(16, n - row)))
                print(f'{a + row:08X} {bs.hex(" ")}')


main()
