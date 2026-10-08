#!/usr/bin/env python3
"""Per-game function registries: what we know about disassembled functions.

Registries live in syms/names/<game>.toml for game in rush1 (SF Rush), rush2 (Rush 2: Extreme Racing USA) and
rush2049 (Rush 2049). Addresses are per game; they are NOT comparable across games. Each entry:

    [[func]]
    vram   = 0x80076854          # required, unique per registry
    name   = "car_set_pedals"    # optional readable base name, snake_case; need not be unique (identical stubs share
                                 # one), the symbol is. Symbol = <name>_<VRAM> (rush2 only matters:
                                 # that is what gen_syms.py will emit). Omit when the purpose is not certain.
    sig    = "void car_set_pedals(Car *car, Player *pl)"   # C-style prototype with named arguments (best knowledge)
    desc   = "one or two lines"  # what it does, args/regs, struct offsets touched
    consts = "0x3C = frames per second; 0x16 = car type count"   # meaning of the magic numbers it uses
    status = "verified"          # "verified" (confirmed in code/data/test) or "inferred" (educated guess)
    ported = true                # true if src/ or us.toml reimplements/patches it
    rush2  = 0x80077518          # rush1/rush2049 only: the equivalent Rush 2 function, if matched
    rush1 / rush2049 = [0x..]    the same function in the other games, as lists (several copies can map to one function).
                                 Derived: `names.py backlinks` fills them from the `rush2` links, creating a stub entry
                                 in the rush2 registry for every linked Rush 2 function. Do not edit by hand.
    link   = "same-code"         # how the rush2 link was established: "same-code" (near-identical code, checked with
                                 # tools/rush2049/verify_names.py) or "role" (same job, code rewritten or inlined)
    area   = "race"              # free-form topic: audio, car, collision, menu, race, render, track, ui, os, ...
    ref    = "docs/x.md#heading" # exact place of the long-form notes: docs/*.md#github-heading-slug, or a plain file path
                                 # for source/us.toml (optional; `names.py anchors` fills and checks the #fragment)

Global variables of Rush 2 live in syms/names/rush2_data.toml as [[var]] entries: vram, name (snake_case; the symbol
is <name>_<VRAM> like functions), type (C type, e.g. "s16", "Car *[8]", "struct { ... }"), desc, area, status, ref.
lookup/search/check/import/index cover them too (import --data).

Commands (run from anywhere):
    names.py lookup ADDR_OR_NAME [--game G]   show entries (ADDR hex, e.g. 80076854; searches all games unless --game)
    names.py search TEXT [--game G]           substring search over name/desc/area
    names.py add GAME ADDR [--name N] [--desc D] [--sig S] [--consts C] [--status S] [--ported] [--rush2 ADDR] [--area A] [--ref R]
                                              upsert one entry (fields given replace the old ones)
    names.py merge GAME FILE.toml             merge a fragment (same format) into a registry: new entries are added; for
                                              existing ones missing fields are filled and a differing desc is appended
    names.py anchors [--game G]               set every docs/*.md ref to the heading (#slug) of the first mention
                                              of the address in that doc; other refs are plain file paths
    names.py backlinks                        derive rush1/rush2049 lists from the rush2 links (all games, both ways)
    names.py rename [--apply]                 give the build the registry names: rewrite func_XXXXXXXX -> <name>_XXXXXXXX
                                              (rush2 registry entries that have a name) in syms/rush2.us.syms.toml, us.toml
                                              and the CODE of src/ include/ patches/ (comments and docs are left alone,
                                              since they mention other games' addresses too). Dry run without --apply.
                                              Idempotent; also verifies no dangling func_ reference is left.
    names.py import FILE.jsonl [--game G] [--data] [--replace]
                                              apply a naming batch (one JSON object per line: vram (hex string), name,
                                              sig, desc, consts, area): fills name/sig/consts/area where missing and the
                                              desc when the entry has none or only the cross-game stub; new entries are
                                              status "inferred".
    names.py index [--game G]                 regenerate docs/functions/<game>_<area>.md (one table per area: address,
                                              symbol, signature, description, constants) and point every entry that has
                                              no other ref at its table
    names.py check                            validate every registry (exit 1 on problems)
    names.py stats                            counts per game / status / area
    names.py unnamed GAME                     entries with a desc but no name (candidates for naming)
    names.py fmt                              rewrite all registries in canonical sorted form
"""
import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIR = ROOT / "syms" / "names"
GAMES = ("rush1", "rush2", "rush2049")
FIELDS = ("vram", "name", "sig", "desc", "consts", "status", "ported", "rush2", "rush1", "rush2049", "link", "area", "ref")
LIST_FIELDS = ("rush1", "rush2049")
LINKS = ("same-code", "role")
STATUSES = ("verified", "inferred")
NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DATA_FILE = DIR / "rush2_data.toml"
DATA_FIELDS = ("vram", "name", "type", "desc", "area", "status", "ref")
HEADER = (
    "# Function registry for %s. Format and tooling: tools/names.py (docstring). Keep sorted; run `names.py fmt`.\n"
)


def path(game):
    return DIR / (game + ".toml")


def load(game):
    p = path(game)
    if not p.exists():
        return []
    with open(p, "rb") as f:
        return tomllib.load(f).get("func", [])


def load_data():
    if not DATA_FILE.exists():
        return []
    with open(DATA_FILE, "rb") as f:
        return tomllib.load(f).get("var", [])


def dump_data(vars_):
    out = ["# Global variable registry for Rush 2 (rush2). Format and tooling: tools/names.py (docstring).\n"]
    for e in sorted(vars_, key=lambda e: e["vram"]):
        out.append("\n[[var]]\n")
        for k in DATA_FIELDS:
            if k in e:
                out.append(("vram = 0x%08X\n" % e[k]) if k == "vram" else "%s = %s\n" % (k, q(e[k])))
    DATA_FILE.write_text("".join(out), encoding="utf-8", newline="\n")


def show_var(e):
    print("%-8s %08X  %s  %s" % ("data", e["vram"], "%s_%08X" % (e["name"], e["vram"]) if "name" in e else "D_%08X" % e["vram"],
                                 e.get("type", "")))
    for k in ("status", "area", "ref"):
        if k in e:
            print("    %-8s %s" % (k, e[k]))
    if "desc" in e:
        print("    " + e["desc"])


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def dump(game, funcs):
    out = [HEADER % game]
    for e in sorted(funcs, key=lambda e: e["vram"]):
        out.append("\n[[func]]\n")
        for k in FIELDS:
            if k not in e:
                continue
            v = e[k]
            if k in ("vram", "rush2"):
                out.append("%s = 0x%08X\n" % (k, v))
            elif k in LIST_FIELDS:
                out.append("%s = [%s]\n" % (k, ", ".join("0x%08X" % x for x in sorted(v))))
            elif k == "ported":
                out.append("ported = %s\n" % ("true" if v else "false"))
            else:
                out.append("%s = %s\n" % (k, q(v)))
    DIR.mkdir(parents=True, exist_ok=True)
    path(game).write_text("".join(out), encoding="utf-8", newline="\n")


def sym(game, e):
    return "%s_%08X" % (e["name"], e["vram"]) if "name" in e else "func_%08X" % e["vram"]


def show(game, e):
    print("%-8s %08X  %s" % (game, e["vram"], sym(game, e)))
    for k in ("status", "area", "ported", "rush2", "rush1", "rush2049", "ref"):
        if k in e:
            v = e[k]
            if k == "rush2":
                v = "0x%08X" % v
            elif k in LIST_FIELDS:
                v = " ".join("0x%08X" % x for x in v)
            print("    %-8s %s" % (k, v))
    if "sig" in e:
        print("    sig      " + e["sig"])
    if "desc" in e:
        print("    " + e["desc"])
    if "consts" in e:
        print("    consts   " + e["consts"])


def parse_addr(s):
    return int(s.lower().replace("func_", "").split("_")[-1] if "_" in s else s, 16)


def cmd_lookup(a):
    games = [a.game] if a.game else GAMES
    key = a.key
    try:
        addr = parse_addr(key)
    except ValueError:
        addr = None
    n = 0
    for g in games:
        for e in load(g):
            if (addr is not None and e["vram"] == addr) or e.get("name") == key or sym(g, e) == key:
                show(g, e)
                n += 1
            elif addr is not None and g != "rush2" and e.get("rush2") == addr and not a.game:
                show(g, e)
                n += 1
    if not a.game or a.game == "rush2":
        for e in load_data():
            if (addr is not None and e["vram"] == addr) or e.get("name") == key or \
                    "%s_%08X" % (e.get("name"), e["vram"]) == key:
                show_var(e)
                n += 1
    if not n:
        print("no entry for %s" % key)
        return 1


def cmd_search(a):
    t = a.text.lower()
    for g in [a.game] if a.game else GAMES:
        for e in load(g):
            if t in " ".join(str(e.get(k, "")) for k in ("name", "sig", "desc", "consts", "area", "ref")).lower():
                show(g, e)
    if not a.game or a.game == "rush2":
        for e in load_data():
            if t in " ".join(str(e.get(k, "")) for k in ("name", "type", "desc", "area")).lower():
                show_var(e)


def cmd_add(a):
    funcs = load(a.game)
    addr = parse_addr(a.addr)
    e = next((x for x in funcs if x["vram"] == addr), None)
    if e is None:
        e = {"vram": addr}
        funcs.append(e)
    if a.name:
        e["name"] = a.name
    if a.desc:
        e["desc"] = a.desc
    if a.status:
        e["status"] = a.status
    if a.ported:
        e["ported"] = True
    if a.rush2:
        e["rush2"] = parse_addr(a.rush2)
    if a.area:
        e["area"] = a.area
    if a.sig:
        e["sig"] = a.sig
    if a.consts:
        e["consts"] = a.consts
    if a.ref:
        e["ref"] = a.ref
    dump(a.game, funcs)
    problems = check(quiet=True)
    for p in problems:
        print("PROBLEM:", p)
    show(a.game, e)
    return 1 if problems else 0


def check(quiet=False):
    problems = []
    for g in GAMES:
        seen, names = {}, {}
        for e in load(g):
            v = e.get("vram")
            if not isinstance(v, int):
                problems.append("%s: entry without integer vram: %r" % (g, e))
                continue
            tag = "%s %08X" % (g, v)
            unknown = set(e) - set(FIELDS)
            if unknown:
                problems.append("%s: unknown fields %s" % (tag, sorted(unknown)))
            if v in seen:
                problems.append("%s: duplicate address" % tag)
            seen[v] = e
            if not (0x80000000 <= v < 0x80800000) or v & 3:
                problems.append("%s: address not a word-aligned KSEG0 address" % tag)
            if "name" in e:
                if not NAME_RE.match(e["name"]):
                    problems.append("%s: name %r must be snake_case" % (tag, e["name"]))
                if re.search(r"_[0-9a-f]{8}$", e["name"], re.I):
                    problems.append("%s: name %r already ends in an address (the tool appends it)" % (tag, e["name"]))
                names[e["name"]] = v
            for k in LIST_FIELDS:
                if k in e and (not isinstance(e[k], list) or not all(isinstance(x, int) for x in e[k]) or k == g):
                    problems.append("%s: %s must be a list of addresses (and not the registry's own game)" % (tag, k))
            if "link" in e and (e["link"] not in LINKS or "rush2" not in e):
                problems.append("%s: link must be one of %s and needs a rush2 address" % (tag, LINKS))
            if "status" in e and e["status"] not in STATUSES:
                problems.append("%s: status must be one of %s" % (tag, STATUSES))
            if "desc" not in e and "name" not in e:
                problems.append("%s: entry has neither name nor desc" % tag)
            if "ref" in e:
                base, _, frag = e["ref"].partition("#")
                fp = ROOT / base
                if not fp.is_file():
                    problems.append("%s: ref file %s does not exist" % (tag, base))
                elif not frag and base.endswith(".md"):
                    problems.append("%s: ref %s has no #fragment (run `names.py anchors`)" % (tag, e["ref"]))
                elif frag and not base.endswith(".md"):
                    problems.append("%s: ref %s: only markdown docs take a #heading fragment" % (tag, e["ref"]))
                elif frag and frag not in {sl for _, sl in md_headings(fp.read_text(encoding="utf-8", errors="replace"))}:
                    problems.append("%s: ref heading #%s not found in %s" % (tag, frag, base))
            if "rush2" in e and g == "rush2":
                problems.append("%s: rush2 link is meaningless in the rush2 registry" % tag)
    seen = set()
    for e in load_data():
        v = e.get("vram")
        tag = "rush2_data %08X" % (v or 0)
        if not isinstance(v, int) or v in seen:
            problems.append("%s: missing or duplicate vram" % tag)
        seen.add(v)
        if set(e) - set(DATA_FIELDS):
            problems.append("%s: unknown fields %s" % (tag, sorted(set(e) - set(DATA_FIELDS))))
        if "name" in e and not NAME_RE.match(e["name"]):
            problems.append("%s: name %r must be snake_case" % (tag, e["name"]))
        if "ref" in e:
            base, _, frag = e["ref"].partition("#")
            fp = ROOT / base
            if not fp.is_file() or (frag and frag not in {sl for _, sl in md_headings(fp.read_text(encoding="utf-8", errors="replace"))}):
                problems.append("%s: bad ref %s" % (tag, e["ref"]))
    if not quiet:
        print("OK" if not problems else "\n".join(problems))
    return problems


def cmd_stats(a):
    for g in GAMES:
        f = load(g)
        by = {}
        for e in f:
            by[e.get("status", "unset")] = by.get(e.get("status", "unset"), 0) + 1
        print("%-9s %4d entries, %3d named, %3d ported, %3d linked to rush2  %s" % (
            g, len(f), sum("name" in e for e in f), sum(bool(e.get("ported")) for e in f),
            sum("rush2" in e for e in f), by))
        areas = {}
        for e in f:
            areas[e.get("area", "-")] = areas.get(e.get("area", "-"), 0) + 1
        print("          areas:", dict(sorted(areas.items())))


def cmd_merge(a):
    with open(a.file, "rb") as f:
        frag = tomllib.load(f).get("func", [])
    funcs = load(a.game)
    by = {e["vram"]: e for e in funcs}
    added = merged = 0
    for n in frag:
        e = by.get(n["vram"])
        if e is None:
            funcs.append(n)
            by[n["vram"]] = n
            added += 1
            continue
        merged += 1
        for k, v in n.items():
            if k not in e:
                e[k] = v
            elif k == "desc" and v not in e[k]:
                e[k] = e[k] + " | " + v
            elif k == "status" and v == "verified":
                e[k] = v
            elif k == "ported" and v:
                e[k] = True
    dump(a.game, funcs)
    print("%s: %d added, %d merged into existing" % (a.game, added, merged))
    return 1 if check() else 0


def slugify(h):
    h = re.sub(r"`|\*\*|\*|_(?=\W|$)|\[|\]\([^)]*\)", "", h).strip().lower()
    h = re.sub(r"[^\w\- ]", "", h)
    return h.replace(" ", "-")


def md_headings(text):
    """[(line_no, slug)] with GitHub's -1/-2 suffixes for repeated headings."""
    out, seen, fence = [], {}, False
    for i, ln in enumerate(text.split("\n"), 1):
        if ln.lstrip().startswith("```"):
            fence = not fence
        m = None if fence else re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", ln)
        if m:
            sl = slugify(m.group(1))
            n = seen.get(sl, 0)
            seen[sl] = n + 1
            out.append((i, sl if n == 0 else "%s-%d" % (sl, n)))
    return out


def anchor_for(ref, vram, name=None, alt=None):
    """ref with its #heading fragment recomputed (markdown docs only) for the first mention of vram (or the symbol
    name, or the linked rush2 address `alt`) in the file. Non-markdown refs are plain file paths."""
    base = ref.split("#")[0]
    f = ROOT / base
    if not f.is_file():
        return None
    if not base.endswith(".md"):
        return base
    text = f.read_text(encoding="utf-8", errors="replace")
    pats = [r"(?<![0-9A-Fa-f])%08X(?![0-9A-Fa-f])" % v for v in (vram, alt) if v]
    if name:
        pats.insert(0, r"\b%s_%08X\b" % (re.escape(name), vram))
    rx = re.compile("|".join(pats), re.I)
    lines = text.split("\n")
    hit = next((i for i, ln in enumerate(lines, 1) if rx.search(ln)), None)
    if hit is None:
        frag = ref.partition("#")[2]
        return ref if frag and frag in {sl for _, sl in md_headings(text)} else base
    slug = None
    for ln, sl in md_headings(text):
        if ln <= hit:
            slug = sl
    return base + ("#" + slug if slug else "")


def cmd_anchors(a):
    changed = 0
    for g in [a.game] if a.game else GAMES:
        funcs = load(g)
        for e in funcs:
            if "ref" not in e:
                continue
            new = anchor_for(e["ref"], e["vram"], e.get("name"), e.get("rush2"))
            if new is None:
                print("%s %08X: ref file missing: %s" % (g, e["vram"], e["ref"]))
            elif new != e["ref"]:
                e["ref"] = new
                changed += 1
        dump(g, funcs)
    print("%d refs updated" % changed)


def cmd_backlinks(a):
    reg = {g: load(g) for g in GAMES}
    by = {g: {e["vram"]: e for e in reg[g]} for g in GAMES}
    for g in GAMES:
        for e in reg[g]:
            for k in LIST_FIELDS:
                e.pop(k, None)
    made = 0
    fwd = {}  # rush2 vram -> {game: [vram]}
    for g in ("rush1", "rush2049"):
        for e in reg[g]:
            if "rush2" in e:
                fwd.setdefault(e["rush2"], {}).setdefault(g, []).append(e["vram"])
    for t, d in fwd.items():
        e = by["rush2"].get(t)
        if e is None:
            e = {"vram": t, "status": "inferred",
                 "desc": "Has counterparts in the other games (see rush1/rush2049); purpose not yet worked out."}
            reg["rush2"].append(e)
            by["rush2"][t] = e
            made += 1
        for g, lst in d.items():
            e[g] = sorted(lst)
        for g in ("rush1", "rush2049"):
            other = "rush2049" if g == "rush1" else "rush1"
            if g in d and other in d:
                for v in d[g]:
                    by[g][v][other] = sorted(d[other])
    for g in GAMES:
        dump(g, reg[g])
    print("%d stub rush2 entries created; %d rush2 entries now link to other games" % (made, len(fwd)))
    return 1 if check() else 0


# ---- renaming the build to the registry names ---------------------------------------------------------------------

FUNC_RE = re.compile(r"\bfunc_([0-9A-Fa-f]{8})\b")
CODE_DIRS = ("src", "include", "patches")
CODE_EXT = (".cpp", ".c", ".h", ".hpp", ".inc")


def symbol_map():
    """{'func_80076854': 'car_set_pedals_80076854'} for every rush2 registry entry that has a name, plus every older
    symbol of that address still in the syms file (so renaming a named function again propagates too)."""
    smap = {"func_%08X" % e["vram"]: "%s_%08X" % (e["name"], e["vram"]) for e in load("rush2") if "name" in e}
    by_addr = {k[5:]: v for k, v in smap.items()}
    sf = ROOT / "syms" / "rush2.us.syms.toml"
    if sf.exists():
        for m in re.finditer(r'name = "([A-Za-z_][A-Za-z0-9_]*_([0-9A-F]{8}))"', sf.read_text()):
            new = by_addr.get(m.group(2))
            if new and m.group(1) != new:
                smap[m.group(1)] = new
    return smap


SYM_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*_[0-9A-Fa-f]{8}\b")


def rename_in(text, smap):
    """Rename func_ / outdated <name>_ADDR identifiers in a piece of code (no comment handling)."""
    def sub(m):
        t = m.group(0)
        if t.startswith("func_"):
            t = "func_" + t[5:].upper()
        return smap.get(t, m.group(0))
    return SYM_RE.sub(sub, text)


def rename_c(text, smap):
    """Rename in C/C++ source, skipping // and /* */ comments (string literals count as code: hook text is code)."""
    out, i, n = [], 0, len(text)
    code_start = 0
    def flush(end):
        out.append(rename_in(text[code_start:end], smap))
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif text.startswith("//", i):
            flush(i)
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j]); i = code_start = j
        elif text.startswith("/*", i):
            flush(i)
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j]); i = code_start = j
        else:
            i += 1
    flush(n)
    return "".join(out)


def rename_toml(text, smap):
    """Rename in us.toml: whole-line comments are kept; a trailing # comment (outside quotes) is kept."""
    out = []
    for ln in text.split("\n"):
        if ln.lstrip().startswith("#"):
            out.append(ln)
            continue
        inq, cut = False, len(ln)
        for k, ch in enumerate(ln):
            if ch == '"' and (k == 0 or ln[k - 1] != "\\"):
                inq = not inq
            elif ch == "#" and not inq:
                cut = k
                break
        out.append(rename_in(ln[:cut], smap) + ln[cut:])
    return "\n".join(out)


def rename_syms(text, smap):
    return re.sub(r'name = "([A-Za-z_][A-Za-z0-9_]*_[0-9A-Fa-f]{8})"', lambda m: 'name = "%s"' % smap.get(m.group(1), m.group(1)), text)


def cmd_rename(a):
    smap = symbol_map()
    targets = [(ROOT / "syms" / "rush2.us.syms.toml", rename_syms), (ROOT / "us.toml", rename_toml)]
    for d in CODE_DIRS:
        for f in sorted((ROOT / d).rglob("*")):
            if f.suffix in CODE_EXT:
                targets.append((f, rename_c))
    total = 0
    for f, fn in targets:
        raw = f.read_bytes().decode("utf-8", errors="surrogateescape")
        new = fn(raw, smap)
        if new != raw:
            cnt = sum(1 for x, y in zip(SYM_RE.findall(raw), SYM_RE.findall(new)) if x != y)
            total += cnt
            print("%-40s %d identifiers renamed" % (f.relative_to(ROOT).as_posix(), cnt))
            if a.apply:
                f.write_bytes(new.encode("utf-8", errors="surrogateescape"))
    print("%d renames in total%s" % (total, "" if a.apply else " (dry run; pass --apply)"))
    return verify_symbols()


def verify_symbols():
    """Every func_/<name>_ADDR identifier used in us.toml keys and C code must be a symbol of the syms file."""
    syms = set(re.findall(r'name = "([^"]+)", vram', (ROOT / "syms" / "rush2.us.syms.toml").read_text()))
    bad = []
    toml = (ROOT / "us.toml").read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r'^func = "([^"]+)"', toml, re.M):
        if m.group(1) not in syms:
            bad.append("us.toml: func = %s is not a symbol" % m.group(1))
    for m in re.finditer(r'^    "([A-Za-z_][A-Za-z0-9_]*)",', toml, re.M):
        if m.group(1) not in syms and not m.group(1).startswith("rush2_"):
            bad.append("us.toml: list entry %s is not a symbol" % m.group(1))
    smap = symbol_map()
    old = set(smap)
    for d in CODE_DIRS:
        for f in (ROOT / d).rglob("*"):
            if f.suffix in CODE_EXT:
                stripped = re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read_text(encoding="utf-8", errors="replace"), flags=re.S)
                for m in SYM_RE.finditer(stripped):
                    t = m.group(0)
                    t = "func_" + t[5:].upper() if t.startswith("func_") else t
                    if t in old:
                        bad.append("%s: still uses old name %s" % (f.relative_to(ROOT).as_posix(), m.group(0)))
    for b in bad[:30]:
        print("DANGLING:", b)
    print("verify: OK" if not bad else "verify: %d problems" % len(bad))
    return 1 if bad else 0


STUB_DESC = "Has counterparts in the other games"
INDEX_DIR = ROOT / "docs" / "functions"


BRITISH = [("colour", "color"), ("behaviour", "behavior"), ("initialis", "initializ"), ("normalis", "normaliz"),
           ("finalis", "finaliz"), ("serialis", "serializ"), ("synchronis", "synchroniz"), ("centre", "center"),
           ("analyse", "analyze"), ("optimis", "optimiz"), ("organis", "organiz"), ("recognis", "recogniz"),
           ("customis", "customiz"), ("favourite", "favorite"), ("honour", "honor"), ("neighbour", "neighbor"),
           ("grey", "gray"), ("utilis", "utiliz"), ("randomis", "randomiz"), ("minimis", "minimiz"),
           ("maximis", "maximiz"), ("visualis", "visualiz"), ("labelled", "labeled"), ("travelled", "traveled")]


def american(t):
    """CLAUDE.md: American spelling. Applied to every text field a batch brings in."""
    for b, am in BRITISH:
        t = t.replace(b, am).replace(b.capitalize(), am.capitalize())
    return t


def batch_lines(fn):
    import json
    for ln in open(fn, encoding="utf-8"):
        ln = ln.strip()
        if ln:
            j = json.loads(ln)
            yield {k: american(v) if isinstance(v, str) and k != "vram" else v for k, v in j.items()}


def cmd_import(a):
    import json
    if a.data:
        return import_data(a)
    funcs = load(a.game)
    by = {e["vram"]: e for e in funcs}
    used = {e["name"]: e["vram"] for e in funcs if "name" in e}
    added = updated = clashes = 0
    for j in batch_lines(a.file):
        v = int(str(j["vram"]).lower().replace("0x", "").replace("func_", ""), 16)
        e = by.get(v)
        if e is None:
            e = {"vram": v, "status": "inferred"}
            funcs.append(e)
            by[v] = e
            added += 1
        else:
            updated += 1
        n = j.get("name")
        if n and "name" not in e:
            n = re.sub(r"_[0-9a-fA-F]{8}$", "", n)
            if not NAME_RE.match(n):
                print("bad name %r for %08X" % (n, v))
            else:
                e["name"] = n
                used[n] = v
        if a.replace and n and e.get("status") != "verified" and NAME_RE.match(re.sub(r"_[0-9a-fA-F]{8}$", "", n)):
            e["name"] = re.sub(r"_[0-9a-fA-F]{8}$", "", n)
        d = j.get("desc")
        if d and ("desc" not in e or e["desc"].startswith(STUB_DESC) or (a.replace and e.get("status") != "verified")):
            e["desc"] = d
        for k in ("sig", "consts", "area"):
            if j.get(k) and (k not in e or a.replace):
                e[k] = j[k]
        e.setdefault("status", "inferred")
    dump(a.game, funcs)
    print("%s: %d added, %d updated, %d name clashes" % (a.game, added, updated, clashes))
    return 1 if clashes else 0


def import_data(a):
    import json
    vars_ = load_data()
    by = {e["vram"]: e for e in vars_}
    added = updated = 0
    for j in batch_lines(a.file):
        v = int(str(j["vram"]).lower().replace("0x", "").replace("d_", ""), 16)
        e = by.get(v)
        if e is None:
            e = by[v] = {"vram": v, "status": "inferred"}
            vars_.append(e)
            added += 1
        else:
            updated += 1
        n = re.sub(r"_?%08x$" % v, "", str(j.get("name", "")).lower())
        if n and NAME_RE.match(n) and ("name" not in e or a.replace):
            e["name"] = n
        for k in ("type", "desc", "area"):
            if j.get(k) and (k not in e or a.replace):
                e[k] = j[k]
    dump_data(vars_)
    print("rush2_data: %d added, %d updated" % (added, updated))


def cell(t):
    return str(t).replace("|", "\\|").replace("\n", " ")


def cmd_index(a):
    INDEX_DIR.mkdir(parents=True, exist_ok=True)
    for g in [a.game] if a.game else GAMES:
        funcs = load(g)
        areas = sorted({e.get("area", "other") for e in funcs})
        for old in INDEX_DIR.glob(g + "_*.md"):
            if old.stem[len(g) + 1:] not in areas:
                old.unlink()
        for ar in areas:
            fp = INDEX_DIR / ("%s_%s.md" % (g, ar))
            title = "%s %s functions" % ({"rush1": "SF Rush", "rush2": "Rush 2", "rush2049": "Rush 2049"}[g], ar)
            out = ["# %s\n\n" % title,
                   "Generated by `tools/names.py index` from syms/names/%s.toml; edit the registry, not this file.\n"
                   "Names and signatures marked inferred are educated guesses from the code.\n\n" % g,
                   "| Address | Symbol | Signature | Description | Constants | Status |\n|---|---|---|---|---|---|\n"]
            for e in funcs:
                if e.get("area", "other") != ar:
                    continue
                out.append("| %08X | `%s` | %s | %s | %s | %s |\n" % (
                    e["vram"], sym(g, e), "`%s`" % cell(e["sig"]) if "sig" in e else "", cell(e.get("desc", "")),
                    cell(e.get("consts", "")), e.get("status", "")))
                if "ref" not in e or e["ref"].startswith("docs/functions/"):
                    e["ref"] = "docs/functions/%s_%s.md#%s" % (g, ar, slugify(title))
            fp.write_text("".join(out), encoding="utf-8", newline="\n")
        dump(g, funcs)
        print("%s: %d entries indexed in %d area files" % (g, len(funcs), len(areas)))
    vars_ = load_data()
    if vars_ and (not a.game or a.game == "rush2"):
        title = "Rush 2 global variables"
        out = ["# %s\n\n" % title,
               "Generated by `tools/names.py index` from syms/names/rush2_data.toml; edit the registry, not this file.\n\n",
               "| Address | Symbol | Type | Description | Area | Status |\n|---|---|---|---|---|---|\n"]
        for e in sorted(vars_, key=lambda e: e["vram"]):
            out.append("| %08X | `%s` | %s | %s | %s | %s |\n" % (
                e["vram"], "%s_%08X" % (e["name"], e["vram"]) if "name" in e else "D_%08X" % e["vram"],
                "`%s`" % cell(e["type"]) if "type" in e else "", cell(e.get("desc", "")), e.get("area", ""), e.get("status", "")))
            if "ref" not in e or e["ref"].startswith("docs/functions/"):
                e["ref"] = "docs/functions/rush2_globals.md#" + slugify(title)
        (INDEX_DIR / "rush2_globals.md").write_text("".join(out), encoding="utf-8", newline="\n")
        dump_data(vars_)
        print("rush2_data: %d globals indexed" % len(vars_))


def cmd_unnamed(a):
    for e in load(a.game):
        if "name" not in e:
            show(a.game, e)


def cmd_fmt(a):
    for g in GAMES:
        if path(g).exists():
            dump(g, load(g))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("lookup"); p.add_argument("key"); p.add_argument("--game", choices=GAMES); p.set_defaults(f=cmd_lookup)
    p = sub.add_parser("search"); p.add_argument("text"); p.add_argument("--game", choices=GAMES); p.set_defaults(f=cmd_search)
    p = sub.add_parser("add"); p.add_argument("game", choices=GAMES); p.add_argument("addr")
    p.add_argument("--name"); p.add_argument("--desc"); p.add_argument("--status", choices=STATUSES)
    p.add_argument("--ported", action="store_true"); p.add_argument("--rush2"); p.add_argument("--area"); p.add_argument("--ref")
    p.add_argument("--sig"); p.add_argument("--consts")
    p.set_defaults(f=cmd_add)
    p = sub.add_parser("merge"); p.add_argument("game", choices=GAMES); p.add_argument("file"); p.set_defaults(f=cmd_merge)
    p = sub.add_parser("anchors"); p.add_argument("--game", choices=GAMES); p.set_defaults(f=cmd_anchors)
    p = sub.add_parser("backlinks"); p.set_defaults(f=cmd_backlinks)
    p = sub.add_parser("rename"); p.add_argument("--apply", action="store_true"); p.set_defaults(f=cmd_rename)
    p = sub.add_parser("check"); p.set_defaults(f=lambda a: 1 if check() else 0)
    p = sub.add_parser("stats"); p.set_defaults(f=cmd_stats)
    p = sub.add_parser("import"); p.add_argument("file"); p.add_argument("--game", choices=GAMES, default="rush2")
    p.add_argument("--data", action="store_true", help="the batch holds global variables (vram, name, type, desc, area)")
    p.add_argument("--replace", action="store_true", help="overwrite name/sig/desc/consts/area (refinement passes; "
                   "the desc of verified entries is kept)")
    p.set_defaults(f=cmd_import)
    p = sub.add_parser("index"); p.add_argument("--game", choices=GAMES); p.set_defaults(f=cmd_index)
    p = sub.add_parser("unnamed"); p.add_argument("game", choices=GAMES); p.set_defaults(f=cmd_unnamed)
    p = sub.add_parser("fmt"); p.set_defaults(f=cmd_fmt)
    a = ap.parse_args()
    sys.exit(a.f(a) or 0)


if __name__ == "__main__":
    main()
