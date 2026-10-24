"""Moves Rush 2's two-player arrays to bigger copies for four players (src/players4.cpp) and eight online.

Each array in MOVES moves to a copy in one 64 KB window of free RDRAM (the old heap area, see src/assets.cpp), so every
relocated `lui` gets the same high half. The script finds the instructions that address the old range (lui +
addiu/load/store pairs, through refscan.py) and prints us.toml `[[patches.instruction]]` entries for the block between
the "4 player relocations" markers in us.toml, plus a review list of luis shared with addresses that don't move.

    python relocate.py scan     sites per array, and the review list
    python relocate.py toml     the patch block
    python relocate.py layout   old -> new addresses
    python relocate.py cpp      the move table of src/players4.cpp
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import refscan  # noqa: E402

WINDOW = 0x80240000

# (old start, old end, new size, meaning). The old range is what moves; the copy is new size bytes.
MOVES = [
    (0x800C2190, 0x800C2268, 0xD8, 'string pointer table after the player records (frees room for records 2-3)'),
    (0x800E7A50, 0x800E7A54, 0x4, 'camera variable inside the extended camera array'),
    (0x800E7A58, 0x800E7AD8, 0x100, 'camera matrices, 0x40 per view'),
    (0x80111888, 0x80111930, 0x150, 'player views, 0x54 per view'),
    (0x8010C040, 0x8010C048, 0x10, 'countdown sign objects, one per view'),
    (0x800C100C, 0x800C1014, 0x10, 'rumble timers, one f32 per player'),
    (0x800D3F68, 0x800D3F78, 0x20, 'rumble state, 8 bytes per player'),
    (0x800E7B9C, 0x800E7B9E, 0x4, 'car followed by each view'),
    (0x800E7B38, 0x800E7B98, 0xC0, 'per-view camera state, 0x30 per view'),
    # Camera code (func_8009AC74-func_8009E138, indexed by the car's view number, car+0x347).
    (0x800E7AF0, 0x800E7AF8, 0x10, 'camera f32 per view'),
    (0x800E7BD0, 0x800E7BD2, 0x4, 'camera byte per view'),
    (0x800E7C20, 0x800E7C28, 0x10, 'camera f32 per view'),
    (0x800E7C38, 0x800E7C50, 0x30, 'camera vector per view'),
    (0x800E7CC0, 0x800E7CC8, 0x10, 'camera f32 per view'),
    (0x800E7D10, 0x800E7D18, 0x10, 'camera f32 per view'),
    (0x800E7D30, 0x800E7D38, 0x10, 'camera f32 per view'),
    (0x800E7D48, 0x800E7D50, 0x10, 'camera f32 per view'),
    (0x800E7B18, 0x800E7B30, 0x30, 'camera vector per view'),
    (0x8010C0F0, 0x8010C108, 0x30, 'camera vector per view'),
    (0x8010C110, 0x8010C128, 0x30, 'camera vector per view'),
    (0x8010C130, 0x8010C138, 0x10, 'camera f32 per view'),
    (0x8010C148, 0x8010C150, 0x10, 'camera f32 per view'),
    (0x80110BE0, 0x80110BF8, 0x30, 'camera vector per view'),
    (0x80112450, 0x80112454, 0x8, 'camera s16 per view'),
    (0x80112458, 0x80112470, 0x30, 'camera vector per view'),
    (0x80112478, 0x80112490, 0x30, 'camera vector per view'),
    (0x80112496, 0x8011249A, 0x8, 'camera byte per view, stride 2'),
    (0x80118A4C, 0x80118A50, 0x8, 'camera s16 per view'),
    (0x80118F78, 0x80118F7C, 0x8, 'camera s16 per view'),
    (0x80118FA0, 0x80118FA8, 0x10, 'camera f32 per view'),
    (0x80118FB0, 0x80118FB8, 0x10, 'camera f32 per view'),
    (0x800BFC74, 0x800BFC7C, 0x10, 'camera f32 per view'),
    (0x800BFC7C, 0x800BFC84, 0x10, 'camera f32 per view'),
    (0x800FAE9C, 0x800FAEA0, 0x8, 'scene node drawn first in each view (s16, -1 for none)'),
    # Race HUD (func_800A06F8, func_8008EFF4, func_800B7B1C).
    (0x800D35E8, 0x800D35F0, 0x20, 'HUD f32 per player'),
    (0x800D3A38, 0x800D3A40, 0x20, 'HUD f32 per player'),
    (0x800D3AF0, 0x800D3AF8, 0x20, 'HUD f32 per player'),
    (0x800D3968, 0x800D3978, 0x40, 'HUD 8 bytes per player'),
    (0x800D39B8, 0x800D39C8, 0x40, 'HUD 8 bytes per player'),
    (0x8010C0D8, 0x8010C0E0, 0x20, 'HUD f32 per player'),
    (0x8010C3DC, 0x8010C3E0, 0x10, 'HUD s16 per player (player 4 would land on the player count)'),
    (0x80125AAA, 0x80125AAC, 0x8, 'HUD byte per player'),
    # Online races have up to eight players, each machine showing one of them (docs/online.md#eight-players): the
    # per-player HUD arrays above hold eight, the globals after the player records move so record 7 fits, and the
    # per-player arrays below that had room for four move to copies for eight.
    (0x800C2268, 0x800C2274, 0xC, 'post-race globals after the player records (frees room for record 7)'),
    (0x800C2274, 0x800C2278, 0x8, 'lap timing byte per player (func_8008EBE0)'),
    (0x800C2278, 0x800C2280, 0x8, 'post-race globals after the player records'),
    (0x800BF318, 0x800BF31C, 0x8, 'place widget shown, byte per player (func_800B5A90)'),
    (0x8010C070, 0x8010C074, 0x8, 'HUD byte per player (func_80060E34)'),
    (0x8010C0B4, 0x8010C0B8, 0x8, 'HUD byte per player (func_800B7654)'),
    (0x8010C0D4, 0x8010C0D8, 0x8, 'HUD byte per player (func_800B7654)'),
    # Stunt scoring per player, two players' worth (car_stunt_scoring_update_80065588, car_helper_80065270,
    # stunt_param_store_800671E8, the stunt HUD func_800B7654 and the race-end stats func_800A9250 are their only
    # users): online every human car scores its stunts (docs/online.md#players-per-machine).
    (0x800E7CB8, 0x800E7CC0, 0x20, 'stunt scoring s32 per player (roll phase)'),
    (0x800E7D08, 0x800E7D10, 0x20, 'stunt scoring s32 per player (scoring on)'),
    (0x800E7D28, 0x800E7D30, 0x20, 'stunt scoring s32 per player (rolls)'),
    (0x800E7D40, 0x800E7D48, 0x20, 'stunt scoring s32 per player'),
    (0x800E7D58, 0x800E7D60, 0x20, 'stunt scoring f32 per player (start time)'),
    (0x800E7DE8, 0x800E7DF0, 0x20, 'stunt scoring f32 per player'),
    (0x800E7DF8, 0x800E7E00, 0x20, 'stunt scoring s32 per player (airborne)'),
    (0x800E7E08, 0x800E7E10, 0x20, 'stunt scoring f32 per player'),
    (0x800E7E18, 0x800E7E20, 0x20, 'stunt scoring f32 per player (landing time)'),
    (0x8010C068, 0x8010C06A, 0x8, 'stunt best u8 per player (func_80065270)'),
    (0x8010C074, 0x8010C078, 0x10, 'stunt points u16 per player (func_80065270, func_800A9250)'),
    (0x8010C0C0, 0x8010C0C2, 0x8, 'stunt multiplier byte per player (func_80065270)'),
    # Sky, per view (sky_setup_800A45A8 builds it, render_sky_cloud_scroll_8008918C moves it each frame, func_800A12A4
    # clears it): Rush 2 built the sky for one player only, the recomp builds it in split screen too (rush2_track49_sky_players).
    (0x80125B68, 0x80125B70, 0x10, 'sky model matrix node per view'),
    (0x80125B90, 0x80125C50, 0x180, 'sky dome polygons, 24 pointers per view'),
    (0x80125C58, 0x80125C60, 0x10, 'sky f32 per view'),
    (0x80125C68, 0x80125C70, 0x10, 'sky f32 per view'),
    (0x80125C78, 0x80125C80, 0x10, 'sky camera x per view'),
    (0x800D0168, 0x800D0170, 0x10, 'sky camera z per view'),
]

# Arrays that move to a given place outside the window (old start, old end, new start, new size, meaning): the race
# HUD's element and widget pools, 200 entries each, which four players' HUDs outgrow (us.toml raises the limits to 400).
PLACED = [
    (0x80119888, 0x8011CA88, 0x802F0000, 0x6400, 'HUD elements, 0x40 each (func_80081008)'),
    (0x8011CA90, 0x8011CDB0, 0x802F6400, 0x640, 'free HUD element pointers (func_80060418)'),
    (0x800F9558, 0x800FAE58, 0x802F6C00, 0x3200, '2D widgets, 0x20 each (func_800541F8)'),
    # The boxes hud_multi_widget_menu_setup_80093B38 builds (0x20 each), 10 of which the stunt panels' boxes and the
    # finished places' boxes outgrow with four players (us.toml raises the count to 20).
    (0x800C4254, 0x800C4394, 0x802FA000, 0x280, 'HUD boxes, 0x20 each (func_80093B38, func_80093A84)'),
    # The scene nodes (0x38 each, scene_node_create_8007F6DC appends with no bound): 1,000 fit before the globals at
    # 0x800E7950, and the arcade Haight's 167 billboards, 80 props and 117 moving objects made 1,045, which ran over
    # text_state_stack_a (0x800E7C50) and crashed its race start. The copy at the bottom of the game heap's old range
    # (the heap now starts at 0x80420000, src/assets.cpp) holds 2,340 (include/scene_nodes.h).
    (0x800D9E90, 0x800E7950, 0x80400000, 0x20000, 'scene nodes, 0x38 each (scene_node_create_8007F6DC)'),
]

# Functions that read the number of players (D_8010C3E2) clamped to 2 instead (rush2::players4::clamped_players):
# per-player rumble and engine sound effects, whose state exists for two players (players 3 and 4 go without), and
# tests of "2 players" that mean "split screen".
NUM_PLAYERS = 0x8010C3E2
CLAMPED_PLAYERS = 0x8024F000
CLAMPED = {
    'func_80067358',  # rumble and engine sound per player
    'func_80067D9C',  # rumble per player
    'func_800663CC',  # car == player 1's, or with 2 players player 2's: rumble / engine sound
    'func_8006722C',  # same
    'func_8009C228',  # camera: split screen adjustments
}

# Instruction sites left alone: addresses that equal a moved array's start but are another array's loop end.
EXCLUDE = {
    '800A375C',  # func_800A3748 fills 0x80118D58/0x80118F80 up to 0x80118F80/0x80118FA0
    # func_800BC888's branch to 0x800BC948 stores through the object func_800BB27C returned ($v0), not the scene-node
    # address the straight-line scan carries over from the lui at 0x800BC914.
    '800BC950',
    '800BC954',
    # The Frame Scale cheat's table (f32 at 0x800BFC80 + cheat byte 0x8010C43C * 4, read by the car select preview
    # func_8008813C and car_matrix_copy_and_apply_8009ABDC) starts inside the camera array 0x800BFC7C, but entries 1-3
    # are past its end: moved, they read zeros and flatten the car (issue 34).
    '800887A8',
    '800887B0',
    '8009AC00',
    '8009AC14',
}

# Loops that run over a whole moved array stop at its old end, which isn't in the moved range: these sites (vram) get
# the new copy's end instead.
LOOP_ENDS = {
    '8007FAF8': 0x800FAE9C,  # func_8007FAB4 sets every view's scene list head to -1
    '8008A584': 0x800E7AF0,  # func_8008A1FC clears a camera f32 per view
    '800A073C': 0x800D3AF0,  # func_800A06F8 clears three HUD f32s per player, up to the third's end
    '80093AB4': 0x800C4254,  # func_80093A84 frees the HUD boxes, up to the pool's end
}


# Bisecting aid: PLAYERS4_SKIP=hex,hex... leaves those arrays out; PLAYERS4_SKIP_CLAMP=1 leaves CLAMPED out.
SKIP = {int(x, 16) for x in os.environ.get('PLAYERS4_SKIP', '').split(',') if x}


def layout():
    out, at = [], WINDOW
    for old, end, size, meaning in MOVES:
        if old in SKIP:
            continue
        out.append((old, end, at, size, meaning))
        at += (size + 15) & ~15
    assert at <= WINDOW + 0x10000
    for old, end, new, size, meaning in PLACED:
        if old not in SKIP:
            out.append((old, end, new, size, meaning))
    return out


def translate(addr):
    for old, end, new, size, meaning in layout():
        if old <= addr < end:
            return new + (addr - old)
    return None


R2 = None


def word(vram):
    global R2
    if R2 is None:
        import roms
        R2 = roms.Rush2()
    return int.from_bytes(R2.read(vram, 4), 'big')


def func_of():
    funcs, cur = {}, None
    for line in open(refscan.ASM):
        line = line.rstrip()
        if line.startswith('func_'):
            cur = line[:-1]
            continue
        m = refscan.line_re.match(line)
        if m and cur:
            funcs[m.group(1)] = cur
    return funcs


def patches():
    allrefs = refscan.scan([(0x80000000, 0x81000000)])
    uses_of_lui = {}
    for f, addr, op, args, val, lui in allrefs:
        uses_of_lui.setdefault(lui, []).append((addr, val))
    out, review, sites = {}, [], {}
    ends = {old: new + size for old, end, new, size, meaning in layout()}
    for f, addr, op, args, val, lui in allrefs:
        new = translate(val)
        if addr in LOOP_ENDS:
            if LOOP_ENDS[addr] not in ends:
                continue
            new = ends[LOOP_ENDS[addr]]
        if f in CLAMPED and val == NUM_PLAYERS and not os.environ.get('PLAYERS4_SKIP_CLAMP'):
            new = CLAMPED_PLAYERS
        if new is None or addr in EXCLUDE:
            continue
        sites.setdefault(f, []).append((addr, op, args, val, new))
        hi = (new + 0x8000) >> 16
        lo = (new - (hi << 16)) & 0xFFFF
        out[addr] = ((word(int(addr, 16)) & 0xFFFF0000) | lo, '%s %s -> %08X' % (op, args, new))
        others = sorted({hex(v) for a, v in uses_of_lui.get(lui, []) if translate(v) is None and a not in LOOP_ENDS and
                         not (f in CLAMPED and v == NUM_PLAYERS)})
        if others:
            review.append('%s %s: lui %s also used for %s' % (f, addr, lui, others))
        elif lui is None:
            review.append('%s %s: no lui found' % (f, addr))
        elif lui not in EXCLUDE:
            out[lui] = ((word(int(lui, 16)) & 0xFFFF0000) | hi, 'lui -> %04X' % hi)
    return out, review, sites


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'scan'
    if cmd == 'layout':
        for old, end, new, size, meaning in layout():
            print('0x%08X-0x%08X -> 0x%08X (0x%X)  %s' % (old, end, new, size, meaning))
        return
    if cmd == 'cpp':
        for old, end, new, size, meaning in layout():
            print('        { 0x%08X, 0x%08X, 0x%08X }, // %s' % (old, end, new, meaning))
        return
    out, review, sites = patches()
    if cmd == 'scan':
        for f in sorted(sites):
            for addr, op, args, val, new in sites[f]:
                print('%s %s %-6s %-28s %08X -> %08X' % (f, addr, op, args, val, new))
        print('REVIEW:')
        print('\n'.join(review))
        print(len(out), 'patches')
    elif cmd == 'toml':
        funcs = func_of()
        for addr in sorted(out):
            v, why = out[addr]
            print('[[patches.instruction]]\nfunc = "%s"\nvram = 0x%s\nvalue = 0x%08X # %s\n' % (funcs[addr], addr, v, why))


if __name__ == '__main__':
    main()
