"""Trackview: a browser preview of custom tracks as the game converts them.

Shows the collision colored by surface, the AI spine and its four racing lines colored by target speed, the race
direction, the checkpoint gates with where each line crosses them, the backward path, and the converter's report
with its positions marked. The track's own glb can be drawn over it.

    python tools/trackview/server.py                       # tracks under build/mods and tmp/stock_tracks,
                                                           # plus San Francisco Rush's and California Speed's (tools/trackview/stock.py)
    python tools/trackview/server.py --track PATH [--track PATH ...] [--port 8766] [--no-browser]

Run AI races the track headless in the real game (build/Rush2Recompiled.exe as a simulation, tools/trackview/runs.py)
and records every car every physics tick; the page replays a recording with play, fast forward, rewind and a
scrubber.

Each track is converted by the game's converter (tools/trackview/bake.py) when the page opens it and again whenever
its files change (the page's Reload button). Zipped mods (.rtz) aren't listed: unzip them first.

See: docs/custom_tracks.md#track-previewer
"""
import argparse, glob, json, mimetypes, os, sys, traceback, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bake, export, runs, stock  # noqa: E402

STATIC = os.path.join(HERE, 'static')
#: Folders searched for track folders (a folder holding track.glb), relative to the repo.
SEARCH = ['build/mods/*/tracks/*', 'build/mods/*/rush2/tracks/*', 'tmp/stock_tracks/*']
EXTRA = []


def tracks():
    """[{folder, name, group}] for every track folder found, extra ones (--track) first."""
    out, seen = [], set()
    found = [(os.path.abspath(p), 'command line') for p in EXTRA]
    for pat in SEARCH:
        for p in sorted(glob.glob(os.path.join(bake.REPO, pat))):
            group = os.path.relpath(os.path.dirname(p), bake.REPO).replace('\\', '/')
            group = group[:-len('/tracks')] if group.endswith('/tracks') else group
            found.append((os.path.abspath(p), group))
    for p, group in found:
        if p in seen or not os.path.exists(os.path.join(p, 'track.glb')):
            continue
        seen.add(p)
        out.append(dict(folder=p, name=os.path.basename(p), group=group))
    return out + stock.listing()


def known(folder):
    """Only folders the track list offers are read."""
    if stock.is_stock(folder):
        return folder if any(t['folder'] == folder for t in tracks()) else None
    folder = os.path.abspath(folder or '')
    return folder if any(t['folder'] == folder for t in tracks()) else None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj, code=200):
        self.send(code, json.dumps(obj, separators=(',', ':')).encode(), 'application/json')

    def do_POST(self):
        # Starting a run launches the game, so it is a POST: no page load, prefetch or reload can start one.
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path == '/run_start':
                folder = known(q.get('folder'))
                if not folder:
                    return self.json({'error': 'unknown track folder'}, 404)
                return self.json(runs.start(folder, backward=q.get('backward') == '1'))
            return self.send(404, b'not found', 'text/plain')
        except Exception:
            return self.json({'error': traceback.format_exc()}, 500)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path == '/tracks':
                return self.json(tracks())
            if u.path == '/bake':
                folder = known(q.get('folder'))
                if not folder:
                    return self.json({'error': 'unknown track folder'}, 404)
                force = q.get('force') == '1'
                res = stock.bake_stock(folder, force) if stock.is_stock(folder) else bake.bake(folder, force)
                if not stock.is_stock(folder) and stock.dump_glb(folder):   # Rush 2 / 2049 stock tracks: the dump's model
                    res = dict(res, has_glb=True, glb_scale=stock.GLB_SCALE)
                return self.json(res)
            if u.path == '/runs':
                folder = known(q.get('folder'))
                if not folder:
                    return self.json({'error': 'unknown track folder'}, 404)
                return self.json({'latest': runs.latest(folder), 'status': runs.status()})
            if u.path == '/run_file':
                folder = known(q.get('folder'))
                path = folder and runs.run_path(folder)
                if not path or not os.path.exists(path):
                    return self.send(404, b'', 'text/plain')
                with open(path, 'rb') as f:
                    return self.send(200, f.read(), 'application/octet-stream')
            if u.path == '/export':
                folder = known(q.get('folder'))
                if not folder:
                    return self.send(404, b'', 'text/plain')
                name, data = export.export(folder)
                self.send_response(200)
                self.send_header('Content-Type', 'application/zip')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Content-Disposition', 'attachment; filename="%s"' % name)
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(data)
                return
            if u.path == '/glb':
                folder = known(q.get('folder'))
                if not folder:
                    return self.send(404, b'', 'text/plain')
                if stock.is_stock(folder):
                    path = stock.glb_path(folder)
                    if not path:
                        return self.send(404, b'', 'text/plain')
                else:
                    path = stock.dump_glb(folder) or os.path.join(folder, 'track.glb')
                with open(path, 'rb') as f:
                    return self.send(200, f.read(), 'model/gltf-binary')
            name = 'index.html' if u.path in ('/', '') else u.path.lstrip('/')
            path = os.path.abspath(os.path.join(STATIC, name))
            if not path.startswith(STATIC) or not os.path.isfile(path):
                return self.send(404, b'not found', 'text/plain')
            with open(path, 'rb') as f:
                return self.send(200, f.read(), mimetypes.guess_type(path)[0] or 'application/octet-stream')
        except Exception:
            return self.json({'error': traceback.format_exc()}, 500)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--track', action='append', default=[], help='a track folder (track.glb + track.toml)')
    ap.add_argument('--port', type=int, default=8766, help='0 picks a free port')
    ap.add_argument('--no-browser', action='store_true')
    a = ap.parse_args()
    EXTRA.extend(a.track)
    srv = ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    url = 'http://127.0.0.1:%d/' % srv.server_address[1]
    if a.track:
        url += '#' + os.path.abspath(a.track[0]).replace('\\', '/')
    print('trackview: ' + url, flush=True)
    if not a.no_browser:
        webbrowser.open(url)
    srv.serve_forever()


if __name__ == '__main__':
    main()
