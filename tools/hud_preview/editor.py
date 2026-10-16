"""HUD editor: place the race HUD's elements by hand (docs/hud_layout.md#hud-editor).

Serves tools/hud_preview/editor.html on http://127.0.0.1:PORT. The page draws the layout engine's result for a layout
and options (tmp/hud_preview/hud_preview.exe --json, with the ROM's images) and lets elements be dragged (snapping to
other elements and the view's edges), anchored, scaled (one element, or every element of the layout by a percentage),
hidden, and sliding ones (WRONG WAY) played. Saving writes src/hud_overrides.inc, which the game and the previewer
build in (rebuild both after saving).

    python tools/hud_preview/editor.py [--port 8765]

Needs the previewer built first: cmd //c "tools\\hud_preview\\build.bat".
"""
import argparse
import json
import os
import re
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PREVIEW = os.path.join(ROOT, 'tmp', 'hud_preview', 'hud_preview.exe')
IMAGES = os.path.join(ROOT, 'tmp', 'hud_preview', 'images')
WORK = os.path.join(ROOT, 'tmp', 'hud_preview', 'editor')
INC = os.path.join(ROOT, 'src', 'hud_overrides.inc')
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'editor.html')

LAYOUTS = {'P1': 'p1', 'P2Stacked': 'p2_stacked', 'P2Side': 'p2_side', 'P3': 'p3', 'P4': 'p4'}
LAYOUT_IDS = {v: k for k, v in LAYOUTS.items()}
# The previewer's --set for each layout.
LAYOUT_SETS = {
    'p1': {'players': '1'},
    'p2_stacked': {'players': '2', 'layout': 'stacked'},
    'p2_side': {'players': '2', 'layout': 'side'},
    'p3': {'players': '3'},
    'p4': {'players': '4'},
}
LINE = re.compile(r'\{\s*LayoutId::(\w+)\s*,\s*Role::(\w+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,'
                  r'\s*(-?\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*([-\d.]+)f?\s*(?:,\s*HudMode::(\w+)\s*'
                  r'(?:,\s*Role::(\w+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*)?)?\}')
ATTACHED = 8  # Override::attached: x, y from the top left corner of (to_role, to_player, to_group)
# The race modes (HudMode, include/hud_layout.h): the editor's Mode box values; 'any' is every mode.
MODES = {'Any': 'any', 'Race': 'race', 'Stunt': 'stunt', 'Battle': 'battle'}
MODE_IDS = {v: k for k, v in MODES.items()}
HEADER = ('// Hand placements of HUD elements, written by the HUD editor (tools/hud_preview/editor.py); see\n'
          '// docs/hud_layout.md#hud-editor. Edit with the editor, not by hand.\n'
          '// { layout, role, player (-1 shared), group, flags (Override::moved 1 | hidden 2 | scaled 4), x, y, origin,\n'
          '//   scale (times its default)[, mode (HudMode; left out: every mode)[, attached (8) to: role, player, group;\n'
          '//   x, y from its top left corner]] }\n')


def read_inc():
    out = []
    if not os.path.exists(INC):
        return out
    with open(INC, encoding='utf-8') as f:
        for line in f:
            if line.lstrip().startswith('//'):
                continue
            m = LINE.search(line)
            if not m:
                continue
            out.append({'layout': LAYOUTS[m[1]], 'role': m[2], 'player': int(m[3]), 'group': int(m[4]),
                        'flags': int(m[5]), 'x': int(m[6]), 'y': int(m[7]), 'origin': int(m[8], 0),
                        'scale': float(m[9]), 'mode': MODES[m[10] or 'Any']})
            if m[11]:
                out[-1].update({'to_role': m[11], 'to_player': int(m[12]), 'to_group': int(m[13])})
    return out


def inc_text(overrides):
    lines = [HEADER]
    order = list(LAYOUTS.values())
    modes = list(MODES.values())
    for o in sorted(overrides, key=lambda o: (order.index(o['layout']), o['role'], o['player'], o['group'],
                                             modes.index(o.get('mode', 'any')))):
        if not o.get('flags'):
            continue
        mode = o.get('mode', 'any')
        tail = '' if mode == 'any' else ', HudMode::' + MODE_IDS[mode]
        if o['flags'] & ATTACHED:
            tail = ', HudMode::%s, Role::%s, %d, %d' % (MODE_IDS[mode], o['to_role'], o['to_player'], o['to_group'])
        lines.append('{ LayoutId::%s, Role::%s, %d, %d, %d, %d, %d, 0x%X, %.3ff%s },\n' % (
            LAYOUT_IDS[o['layout']], o['role'], o['player'], o['group'], o['flags'], round(o['x']), round(o['y']),
            o['origin'], o['scale'], tail))
    return ''.join(lines)


def solve(sets, overrides, name):
    os.makedirs(WORK, exist_ok=True)
    inc = os.path.join(WORK, name + '.inc')
    js = os.path.join(WORK, name + '.json')
    with open(inc, 'w', encoding='utf-8', newline='\n') as f:
        f.write(inc_text(overrides))
    args = [PREVIEW, '--no-picture', '--overrides', inc, '--json', js]
    for k, v in sets.items():
        args += ['--set', '%s=%s' % (k, v)]
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=30)
    if not os.path.exists(js) or r.returncode == 2:
        raise RuntimeError(r.stderr or r.stdout or 'hud_preview failed')
    with open(js, encoding='utf-8') as f:
        return json.load(f)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, kind='application/json'):
        data = body if isinstance(body, bytes) else body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/':
            with open(PAGE, 'rb') as f:
                return self.send(200, f.read(), 'text/html; charset=utf-8')
        if path.startswith('/images/'):
            name = os.path.basename(path[len('/images/'):])
            file = os.path.join(IMAGES, name)
            if os.path.isfile(file):
                with open(file, 'rb') as f:
                    return self.send(200, f.read(), 'image/png')
            return self.send(404, b'', 'text/plain')
        if path == '/overrides':
            return self.send(200, json.dumps(read_inc()))
        self.send(404, b'', 'text/plain')

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        try:
            if self.path == '/solve':
                sets = dict(LAYOUT_SETS[body['layout']])
                sets.update({k: str(v) for k, v in body.get('options', {}).items()})
                # 3 players are 4's layout (the same quadrants): its hand placements are 4's.
                key = 'p4' if body['layout'] == 'p3' else body['layout']
                mine = [o for o in body.get('overrides', []) if o['layout'] == key]
                result = {'layout': solve(sets, mine, 'current'), 'base': solve(sets, [], 'base')}
                return self.send(200, json.dumps(result))
            if self.path == '/save':
                with open(INC, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(inc_text(body['overrides']))
                return self.send(200, json.dumps({'saved': os.path.relpath(INC, ROOT)}))
        except Exception as e:  # shown in the page
            return self.send(500, json.dumps({'error': str(e)}))
        self.send(404, b'', 'text/plain')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8765)
    a = ap.parse_args()
    if not os.path.exists(PREVIEW):
        sys.exit('build the previewer first: cmd //c "tools\\hud_preview\\build.bat"')
    # The ROM images (Rush 2049's too), refreshed every start so a new previewer's are there.
    subprocess.run([PREVIEW, '--dump-images', IMAGES], cwd=ROOT, check=True)
    # Not SO_REUSEADDR: on Windows it lets a second server bind a port already in use, and requests go to either.
    ThreadingHTTPServer.allow_reuse_address = False
    try:
        server = ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    except OSError as e:
        sys.exit('port %d is in use (another editor running?): %s' % (a.port, e))
    print('HUD editor: http://127.0.0.1:%d/  (Ctrl+C to stop)' % a.port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
