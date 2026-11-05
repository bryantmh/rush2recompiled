"""Prints a Rush 2 widget table (the layout entries a screen hands widgets_create_800604FC): each entry's name, x, y,
callback and parameter, read from the recomp ROM (tools/rush2049/roms.py; RUSH2_ROM overrides its path).

Usage:  python tools/menu_widgets.py ADDR COUNT     e.g. 0x803C8730 5 (SETUP's, menu_screen_options_list_803B17A8)

An entry is 0x28 bytes: +0 name pointer (0 none, -1 a box or callback-only entry), +4 x, +6 y (s16), +0x20 callback,
+0x24 parameter (src/hud.cpp rush2_hud_entry_created reads the same fields).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush2049'))
import roms  # noqa: E402

ENTRY = 0x28


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    table, count = int(sys.argv[1], 0), int(sys.argv[2], 0)
    rom = roms.Rush2()
    for i in range(count):
        e = table + i * ENTRY
        p = rom.w(e)
        name = rom.s(p) if 0x80000000 <= p < 0x80800000 else ('-1' if p == 0xFFFFFFFF else hex(p))
        print(f'{i:2} {e:08X} {name:<20} x {rom.h(e + 4):4} y {rom.h(e + 6):4}  callback {rom.w(e + 0x20):08X}'
              f'  param {rom.w(e + 0x24):08X}')


if __name__ == '__main__':
    main()
