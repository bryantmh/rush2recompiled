# Writes the PS1 SF Rush disc (a Redump .cue + .bin set) in the other image formats src/sfrush_ps1/ps1_disc.cpp reads,
# to test the reader and the disc's music offline:
#   python3 tools/sfrush_ps1/ps1_variants.py DISC.cue OUTDIR [CHDMAN.exe]   (about 1.5 GB written; delete afterwards)
#   game.iso      track 1 as 2048-byte sectors (no music)
#   game.img/.ccd CloneCD: every track's sectors in one .img, the .ccd listing the tracks
#   game.chd      chdman createcd of the .cue (only with CHDMAN)
#   single.bin/.cue  every track in one .bin, one FILE line
# Then: tools/sfrush_ps1/cpp_test/build.bat OUT OUTDIR/game.chd (and the others); ps1_test.exe prints the audio tracks.
import os, re, subprocess, sys

cue_path, out = sys.argv[1], sys.argv[2]
chdman = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(out, exist_ok=True)
base = os.path.dirname(cue_path)
SECTOR = 2352

# FILE / TRACK / INDEX from the .cue: per track (number, mode, file, {index: sector in its file}).
tracks, file = [], None
for line in open(cue_path, encoding='utf-8', errors='replace'):
    f = line.split()
    if not f:
        continue
    key = f[0].upper()
    if key == 'FILE':
        file = os.path.join(base, re.search(r'"(.*)"', line).group(1))
    elif key == 'TRACK':
        tracks.append({'n': int(f[1]), 'mode': f[2].upper(), 'file': file, 'idx': {}})
    elif key == 'INDEX':
        m, s, fr = map(int, f[2].split(':'))
        tracks[-1]['idx'][int(f[1])] = (m * 60 + s) * 75 + fr

files = []
for t in tracks:
    if t['file'] not in files:
        files.append(t['file'])
file_start, at = {}, 0
for fp in files:
    file_start[fp] = at
    at += os.path.getsize(fp) // SECTOR

# game.iso: track 1's 2048 bytes of data per sector.
with open(tracks[0]['file'], 'rb') as src, open(os.path.join(out, 'game.iso'), 'wb') as dst:
    while True:
        s = src.read(SECTOR)
        if len(s) < SECTOR:
            break
        dst.write(s[24:24 + 2048])

# game.img + game.ccd, and single.bin + single.cue (the same bytes).
with open(os.path.join(out, 'game.img'), 'wb') as dst:
    for fp in files:
        with open(fp, 'rb') as src:
            while True:
                b = src.read(64 << 20)
                if not b:
                    break
                dst.write(b)
try:
    os.remove(os.path.join(out, 'single.bin'))
except FileNotFoundError:
    pass
os.link(os.path.join(out, 'game.img'), os.path.join(out, 'single.bin'))

ccd = ['[CloneCD]', 'Version=3', '[Disc]', f'TocEntries={len(tracks) + 3}', 'Sessions=1', 'DataTracksScrambled=0',
       'CDTextLength=0', '[Session 1]', 'PreGapMode=2', 'PreGapSubC=1']
cue = ['FILE "single.bin" BINARY']
for t in tracks:
    ccd += [f'[TRACK {t["n"]}]', f'MODE={0 if t["mode"] == "AUDIO" else 2}']
    cue.append(f'  TRACK {t["n"]:02d} {t["mode"]}')
    for i, sec in sorted(t['idx'].items()):
        lba = file_start[t['file']] + sec
        ccd.append(f'INDEX {i}={lba}')
        cue.append(f'    INDEX {i:02d} {lba // 4500:02d}:{lba // 75 % 60:02d}:{lba % 75:02d}')
open(os.path.join(out, 'game.ccd'), 'w', newline='\r\n').write('\n'.join(ccd) + '\n')
open(os.path.join(out, 'single.cue'), 'w', newline='\r\n').write('\n'.join(cue) + '\n')

if chdman:
    chd = os.path.join(out, 'game.chd')
    if os.path.exists(chd):
        os.remove(chd)
    subprocess.run([chdman, 'createcd', '-i', cue_path, '-o', chd], check=True)
print('written to', out)
