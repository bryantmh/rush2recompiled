"""Decompile Rush 1 functions with m2c (research aid). Usage: python decomp1.py FUNCADDR [more...]

Wraps tools/rush2049/decomp.py with Rush 1 function bounds (out/r1.asm, from dis1.py) and memory.
"""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import decomp
from r1 import Rush1

HERE = os.path.dirname(os.path.abspath(__file__))
_r = None

def _bounds(game):
    return sorted(int(m.group(1), 16) for m in re.finditer(r'^func_([0-9A-F]{8}):', open(os.path.join(HERE, 'out', 'r1.asm')).read(), re.M))

def _reader(game):
    global _r
    _r = _r or Rush1()
    return lambda a: struct.unpack('>I', _r.read(a, 4))[0]

decomp.bounds = _bounds
decomp.reader = _reader

if __name__ == '__main__':
    for f in sys.argv[1:]:
        a = int(f.replace('func_', ''), 16)
        asm = decomp.emit('r1', a)
        with tempfile.NamedTemporaryFile('w', suffix='.s', delete=False) as t:
            t.write('.set noat\n.set noreorder\n' + asm)
            path = t.name
        res = subprocess.run([sys.executable, decomp.M2C, '--valid-syntax', path], capture_output=True, text=True)
        print(res.stdout or res.stderr)
        if res.returncode: print(asm)
