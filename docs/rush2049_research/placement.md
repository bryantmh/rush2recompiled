# Track object placement and 2049 animated objects

Research notes for porting Rush 2049 race tracks into Rush 2 Recompiled. Tags: **[V]** = verified in disassembly
or data, **[I]** = inferred. Addresses are virtual addresses (Rush 2 main 0x800539E0, Rush 2049 main 0x80086A50).
Tool: `tools/rush2049/placement.py` (parsers, converter, self-check; run `python placement.py`).

---------------------------------------------------------------------------------------------------------------------

## 1. Rush 2 placement format **[V]**

Asset 0x3F + track, deflate. 100% of the 12 stock files parse and re-encode byte-identically (`write_r2`).

```
u32 count                         always 1
count x { u32 offset; char name[16]; }    offset of the tree's first record; name = track prefix (VEGAS, NYONE...)
records, 0x64 bytes each, until end of file:
  +0x00 char name[16]
  +0x10 f32  m[9]        3x3 rotation*scale; the scene node's matrix pointer (node+4) points HERE
  +0x34 f32  pos[3]      world position (rewritten in place to parent-relative for breakables)
  +0x40 u32  flags       copied to scene node +0 (stock: 0x40, some 0x40040)
  +0x44 s16  next        next sibling record index, -1 = end
  +0x46 s16  child       first child record index, -1 = none
  +0x48 u32  0
  +0x4C f32  bbmin[3], bbmax[3]   culling box (only used for top-level records)
```
`00000040 0001FFFF` in the plan = flags 0x40, next = 1, child = -1. Stock trees are two levels deep: top-level
records are track sections (`NYONECHALL1`, `VEGASL3134`, `<prefix>FINISH`), children are props, sounds, collision
volumes and breakables. No stock top-level record is rotated.

Node flag bits (func_8007B518 / func_8007FC80): 0x400 hidden, 0x100<<view hidden in that view, 0x10/0x20 special
draw, **0x40000 = draw in the late pass** (transparent), 0x380000 = culling-box type and 0xFFC00000 = culling-box index
(written by func_8007FC80). 2049's extra bit 0x400000 lands in the culling index and must be stripped (2049 keeps
its culling box in a separate node word +4, so there it is a free flag: see 2.2).

### 1.1 Loading and instantiation
- **Relocation:** func_800A5110 (race setup, 0x800A5308) adds the file base to every directory offset;
  0x800E7C1C = file, 0x800E7C30 = directory.
- **Tree lookup:** func_800A5110 builds the key from the prefix table 0x800C182C[track] and binary-searches the
  directory (func_8005BCB4, 0x14-byte entries). **The result is used unchecked** (`lw t6,0(v0)` at 0x800A56F0): the
  converted file's tree name must equal the replaced slot's prefix, or the game crashes. func_80081B58 is the same
  lookup by arbitrary name (no direct caller in main).
- Record base → 0x8010C15C; walker **func_80081790(record, parentNode)**:
  1. For every sibling: 0x800D5790 = record, `func_800815DC(name,&type,&extra)` classifies the name, the world
     position is copied to 0x800D0178.
  2. type 5/6 → `func_8007FF44(extra)` = behaviour table entry `extra` called with (-1,-1); no node.
  3. type 1-4 with a parent → position -= parent node position (node+4 → floats +0x24..+0x2C). Only the position,
     not the rotation, so parents must be unrotated.
  4. Skips: type 1 when players ≥ 2 (0x8010C3E2); type 2 (`_F`) when backward (0x80119848 ≠ 0); type 3 (`_B`) when
     not backward.
  5. `handle = func_8005BE3C(debris ? debris : name, 0, slots-1, 1)` (all loaded model slots, first match wins),
     `node = func_8007F6DC(handle, rec+0x10, mode 1|3, parent, rec+0x40)`, `func_8007FFA0(handle, node)` = run the
     named object's behaviour (see 1.3).
  6. Top-level and bbox non-degenerate → `func_8007FC80(bbmin, bbmax, node)` registers a culling box.
  7. Second pass: the k-th sibling's children are walked with parent = (node of the first sibling) + k. **Every
     sibling before a record with children must create exactly one node**, otherwise children attach to the wrong
     parent. Stock data satisfies this because only top-level sections have children.
- Scene node (0x800D9E90, 0x38 bytes, func_8007F6DC): +0 flags, +4 matrix pointer (12 floats: m[9], pos[3]),
  +8 f32 scale 1.0, +0xC u16 model handle, +0xE/+0x10/+0x12 s16 tree links, +0x14..+0x33 0, +0x30/+0x34 colours.
  func_8007B518 re-reads the matrix floats every frame, so **animating a node = rewriting the 12 floats it points at**.

### 1.2 Name classifier func_800815DC **[V]**
| Order | Table | Match | Result |
|---|---|---|---|
| 1 | 0x800C5330, 23 × {name, id}: BIGCHR1/2, BOAT, CABLECAR, CANNON, CROWDSCR, DOCKWHIS, FIRECRCK, FOGHORN, FOUNTAIN, KIDSPLAY, OCEAN, PARKBIRD, SEAGULL, SMLCLAP, SMLHOOT, VOLCANO, KLAX, HARDRIVE, PETERP, PITFIGTER, RAMPART, MARBLE | exact | type 5, extra = id (ambient sound emitters, ids 0x0A-0x30) |
| 2 | 0x800C53E8: MARKER (0), TIME (0), COLLISION (0x21) | prefix | type 5. COLLISION → behaviour 0x21 = func_800BC5F4 → func_800AF810: adds the record (radius from m, position) to the static collision-cylinder list 0x8010D3B8 |
| 3 | 0x800C5400..0x800C55F8, 42 × {name, debris model, s16 flags, s16 sound}: CONE1→CONE1O1, FENCE, FLAG2, GASIGN, GATE, KEY, METER, TREEHIT→TREEHIT1O1, WINDOW, BALL, SHATPANE, CURVEHIT, THINKHIT, BUMPHIT, DIPHIT, PCAREHIT, NOPASHIT, RIGHTHIT, LEFTHIT, ZONEHIT, MPH45HIT, MPH75HIT, YIELDHIT, REDUCHIT, STOPHIT, SLOWHIT, NYTREEHT, GRANDWIN, GLAMPHIT, CHAIRHIT, DESKHIT, MAPSIGN, SRFBRD, USFLAG, UMBRELLA, RATCONE→RATCONEO3, P737, PJET, F23, ENGTABLE, DOTHEDEW, NYLGATE | prefix | type 1; flags&2 → 4; flags&1 → `_B` in name → 3, `_F` → 2. The node is created for the **debris model** (CONE1O1...), 0x80125C54 = the s16 sound |
| - | none | | type 0: plain model, looked up by the record name |

Breakables, keys and dew cans work through the named object's behaviour id (1.3); the record name only selects the
class. All TREEHIT* variants draw TREEHIT1O1 [V: the lookup uses the debris name].

### 1.3 Behaviour ids **[V]**
Named-object records (0x18 bytes: name[16], f32 radius, **s16 behaviour id at +0x14**, s16). func_8008BC64 sets the
behaviour table to 0x800C526C (49 entries). func_8007FFA0(handle,node) calls `table[id-1](handle,node)` when id ≠ 0;
func_8007FF44(id) calls `table[id](-1,-1)`. Examples: CONE1O1 2, *HITO1 signs 5, TREEHIT51-56 6, GLAMPHITO1 7,
KEYO1/DOTHEDEWO1 8, SHATPANEO1/GRANDWINO1 9, FLAG2O1 23, SRFBRDO1 36, UMBRELLAO1 37, RATCONEO3 38, USFLAGO1 39,
CHAIRHIT 40, DESKHIT 41, NYLGATEO1 42. The callbacks (0x800BB7B8...) allocate a breakable instance in 0x800D58E0
(0x6C bytes, max 0x82, func_800BB27C; +0x58/+0x5C hit/update callbacks, +0x62 sound, +0x64 class); func_8008A01C
updates them each frame. Track sections have id 0.

### 1.4 Which models are loaded **[V]**
func_800A4C98 queues track geometry 0x33+t, placement 0x3F+t, collision 0x4B+t, AI path 0x57+t(+12), plus
assets **0x12** (effects, KEYO1, DOTHEDEWO1, MINE1, SMOKE*...) and **0x14** (CONE1O1, FLAG2O1, SHATPANEO1-7,
TREEHIT*, all *HITO1 signs, METERO1). Track-specific debris lives in track geometry: FENCEO1-12 (HAWAII, LA),
GASIGNO1-3 and MAPSIGNO1-3 (LA), F23O1/P737O1/PJETO1/SRFBRDO1/UMBRELLAO1-3/USFLAGO1-3 (HAWAII), CHAIRHIT/DESKHIT/
GLAMPHIT/GRANDWIN/NYTREEHT (NYTWO), NYLGATEO1 (NYONE), RATCONEO3 and SUBWAYO1 (NYONE, NYTWO).
Self-check: all 2544 stock records resolve against {track geometry, 0x12, 0x14}.

### 1.5 Hard-coded names and failure safety
| Site | Names | Missing name |
|---|---|---|
| func_800A5110 (0x800A56E8) | placement tree `<prefix>` (0x800C182C) | **crash** (null result dereferenced) |
| func_80081790 | every placement record (name or debris name) | **unsafe**: F6DC returns -1 safely, but func_8007FFA0(-1,-1) indexes the name table with slot -1 (reads 0x80119010 → probably 0 → load from 0x00005FFC = TLB miss on hardware / out-of-range in the recomp) **[I]**; a top-level one would also register a culling box on node -1 (writes 0x800D9E58) |
| func_8008F080 (from func_8008F220, lap/checkpoint logic [I]) | `<prefix>FINISHB` (backward only, falls back), `<prefix>FINISH` | FINISH: **unsafe**, handle 0xFFFF used unchecked (model record slot 63/index 1023 → its LOD0 display list is patched by func_80077518) |
| same | textures `CHKPNT`, `FINISH` (func_800601F8) | **crash** (`lw 0x18(v0)` on null). They exist in every Rush 2 race geometry (0x33-0x3D), not in shared assets: the converted geometry must contain both textures and a `<prefix>FINISH` model whose display list calls the CHKPNT texture-load DL (func_80077518 swaps that G_DL target for the FINISH one). The stock files also place `<prefix>FINISH` as a top-level placement record |
| func_800A45A8 (sky) | track 0: model `SKYO1`; track 11: `STUNTSKYO1`; others: textures `SKY01`, `SKYFOUR` (asset 0x12) | models: same unsafe path as placement (F6DC -1, then func_8007FFA0(-1)); textures are shared, safe |
| func_800A4210 (tracks **1 and 3 only**, hard-coded at 0x800A4260) | `SUBWAYO1` + hard-coded subway paths 0x800BF710 / 0x800BFBB8 | unsafe: creates subway nodes and pseudo-car physics bodies (0x81C each, 0x800D50E4); avoid replacing slots 1/3 or hook it off |
| func_8008AF48 / func_8008A9C8 | per-track palette-cycling lists via 0x800C5D2C (tracks 0, 1, 3, 5, 11): VEGAS CASINO*/WESHO*/STARSTR/RIVLGHT*/FRMT*/MGMLGHT/MNYCSTL..., NY NYSIGGRN/NYSIGRED/ADCARA/ADCOLA(/DOWJONES), LA LASIGGRN/LASIGRED, STUNT1 ST*/STROAD/UPFLOOR | **unsafe**: a missing texture leaves entry+0x10 unchanged, and func_8008A9C8 rotates palette entries through it without a null check. The table lives in RDRAM and keeps the previous race's pointer, so a stale pointer would scribble on the new heap. **Zero 0x800C5D2C[slot] for a replaced slot** (or point it at a list for the new textures). Entry: name, s16 first, s16 last, s8, u8 mode (<11, jump table 0x800CFB6C), s8 period, s16 timer, +0x10 palette ptr |
| func_8008A1FC (every race) | effect/debris/rim names (0x800C5D5C.., 0x800C6088..) | safe at init: stock tracks already store 0xFFFF for debris they lack; a handle is only used when that breakable class breaks. So only map 2049 objects to breakable classes whose debris model is in 0x12/0x14 |
| func_800AF460 | `RATCONEO3`, textures `RATCONEA`, `BADRAT` | only runs from behaviour 38 (RATCONEO3's own id): safe |
| func_80086700 | 39 car-part names | unsafe (plan §6.2), car topic |

### 1.6 Other Rush 2 moving objects (template for 2049 ones) **[V]**
Subways: func_800A4210 (init) creates one node per car of the train (handle SUBWAYO1, matrix in 0x800D4F00 entries
of 0x4C bytes, count 0x800D4E74) and allocates `count` 0x81C-byte **pseudo-car physics structs** at 0x800D50E4 so the
car-vs-car code makes trains hit players. func_80075FB8 moves them each physics tick; it is called from the physics
step **func_80076578** (with func_800763FC). This is the natural hook point and model for 2049 movers.

---------------------------------------------------------------------------------------------------------------------

## 2. Rush 2049 placement format **[V]**

Files 120 + level (120-125 race tracks 1-6, 126-133 battle DM1-8, 134-137 stunt, 138 obstacle); loader
func_800BB9B0 (file 0x78 + level).
```
u32 dir_offset, u32 chunk_count (4)
WHDR (count 24 bytes): exactly the Rush 2 header {u32 1; u32 offset; char name[16]} (offset relative to the file)
WOBJ (offset, count = records): 0x68-byte records = Rush 2 record with an extra field:
   +0x00..+0x47 as Rush 2;   +0x48 u32 0;   +0x4C s32 dynamic-object id (-1 static);   +0x50 bbox[6]
GTLD: s32 dynamic-object ids, grouped per GDAT leaf
GDAT: 28-byte quadtree nodes over x/z: s16 parent, u16 kind (1 inner, 0x10 leaf), f32 x0,x1,z0,z1,
      inner: s16 child[4];  leaf: u16 count, u16 first GTLD index, u32 0
directory: {char tag[4]; u32 offset; u32 count}
```
- func_800BB9B0 (0x800BBB08): `WHDR` → 0x801497FC (+4 → 0x80149818, offsets relocated), `GDAT` → 0x80149770
  (count 0x80149790), `GTLD` → 0x801497C0 (count 0x801497EC). WOBJ is reached only through WHDR.
- Walker **func_800AC3D8** (same algorithm as func_80081790, stride 0x68, record base 0x80149B80, current record
  0x80149D94): `func_800ABCC8(rec, parent, ...)` handles every dynamic type; otherwise
  `func_80092E2C(name)` + `func_8008E26C(handle, rec+0x10, parent, flags)` + bbox → func_800A7E10.
  func_800AC668 = lookup by name (Rush 2 func_80081B58).
- **Dynamic ids** (+0x4C) number the dynamic placement objects 0..N; PTHD objects continue the numbering
  (PTHD +0x20). func_800B2DF8 replaces every GTLD id by the spawned object's pointer; func_800BEAA0 (per frame) walks
  the GDAT quadtree around each car and calls the nearby objects' update/collision callbacks. GDAT/GTLD is therefore
  a **spatial index of dynamic objects for car-object interaction**; Rush 2 does not need it.
- Direction variants: `_FW` only in forward races, `_BW` only backward (func_800A464C on 0x80121D40 `_BW`,
  0x80121D44 `_FW`, flag 0x80152570). Same idea as Rush 2's `_F`/`_B` (0x80119848), which confirms that
  0x80119848 is Rush 2's **backward** flag **[I]**.
- Flags: 0x40, 0x40040 (late pass, same meaning as Rush 2), 0x400000 (2049-only, no draw-distance culling, see 2.2).
- **2049 OBHD names are char[16]**, compared on 15 characters (func_80095120 → strncmp 15), not char[12]: bytes
  after the NUL are garbage, which made names look truncated.
- All 19 files: every static name resolves in its geometry file (self-check).

### 2.1 Dynamic-object type table 0x80117530 **[V]**
122 entries × 0x30, matched by **prefix in table order** (strncmp with strlen of the entry name, func_800ABCC8):
`+0 name*, +4 model name*, +8 init fn, +0xC update fn, +0x10 u32 type flags, +0x14 s16 flip-book base (-1 none),
+0x16 u8 kind, +0x17 u8 sub-kind, +0x18 f32 param, +0x1C..+0x24 s32 x3 (sounds), +0x28 u32, +0x2C f32 range`.

| Kind | Types | Behaviour |
|---|---|---|
| 0 | BUMPHIT CURVEHIT METER MPH45HIT MPH75HIT NOPARK SLOWHIT STOPHIT THINKHIT (sub 1), GETOFF (2), TROLLEY2 (3), WINDMILL (4), WINDMILL2 (5), WEPICON_* (0, battle) | knock-over signs (init 0x8010E72C, update 0x8010E828); sub 3-5 animate in place (update 0x8010E694) |
| 2 | CONE1 GASPUMP RATCONE RAT | knock-over props (0x8010DCFC / 0x8010E4E4) |
| 4 | path followers (init 0x800C1604, update 0x800C0AC0): BARGE BKWBARIER BLOCK* BOULDER ELEVATOR1 F1FLAG F2FLAG GLASSWALL GONDOLA* GREGDOOR* GREGRAMP JUMP MINITRAIN(N) PILLARS PISTON PLANE(NIGHT) PLATFORM POST RADAR RAMP RAMP02 ROAD ROTOR(2) SHARK SHARKSIT SPIKEDBALL SPOT1 SPOTLIGHT1/2 T1 T2 T3 T3* TDOOR1-8 TEETH(C) TOILET TRAINNIGHT TRAINORG TRAPDOOR2 TRAPDOORT1 TRIGGER TROLLEY TROLLEYNT WALL WALL2 WALL3 WINDMILL3 WINDROTOR1-3 | see §3 |
| 5 | WPR_MINE | battle |
| 6 | GOLDCOIN SILVERCOIN | coins |
| 7 | BULB CACTUS COLLISION FENCE FLAG2 GUARDRAIL SHATPANE YIELDHIT | misc (FENCE 0x8010DAF8, SHATPANE 0x8010DBB8, CACTUS 0x8010D9CC; no-model ones are collision volumes) |

Type flag 0x8000 = **moving collision body** (counted in 0x80150F78, see §3.3): BLOCKNV, BOULDER, ELEVATOR1,
GONDOLA1/2, MINITRAIN(N), PISTON, PLANE, TEETH, TRAINORG (first entry), TROLLEY, TROLLEYNT. Flip-book models:
F1FLAG (20 frames F1FLAGG28-47, file 78), SHARK (4 frames).

Models: per-track objects in files 82-87 (CONE1G1, GASPUMPG1, GONDOLA*, TRAINORGDEFAULT, TROLLEY*, WINDMILL*,
SHARK*, SPOT*, ...), level-specific movers in the track geometry (TRAPDOOR2G1, GLASSWALLG1, T1G1, TDOOR1G1, ...),
shared: 68 coins, 78/79 F1FLAG/F2FLAG/TRIGGEROFF/TRIGGERON, 76 weapons, 61/62 effects. Race-time loads
(func_800BB9B0): 101+t geometry, 120+t placement, 139+t collision, **AI path 158+t forward / 177+t backward**
(0x800BBCD0, flag 0x80152570; so files 177-182 are the backward paths of race tracks 1-6), then 76, 68, 78/79,
82+t, 77, 62, 81, 61, 64, 66.

---------------------------------------------------------------------------------------------------------------------

### 2.2 Node flag 0x400000 = exempt from draw-distance culling **[V]**
2049 scene nodes (table 0x8012E700, stride 0x44) hold the flags at +0 and the culling word at +4
(`func_800A7E10` ORs `(box_index << 3) | 7` into +4, so the culling index is not in the flag word as in Rush 2).
The per-view draw walker `func_8009DD88` (node in $s7, flags in $v1) decodes +4 (`& 7` = box type, `(& 0x1FF8) >> 3`
= box index into 0x80157248, s16 min/max stored x16) and at 0x8009E41C tests `flags << 9` (bit 22 = **0x400000**):
set → branch straight to the draw path (0x8009E51C); clear → compute `dist(camera, node pos) - 0.0625 * box extent`
and, if that exceeds the draw distance, skip the node (0x8009E654). The test only runs when the draw distance
`0x80151AA0` is below 2000.0; that float is the far/cull distance of the current camera mode (2000.0 default in
func_800A5908, set from the table at 0x8011E7A8 by `func_800AB18C`).
- Setter: `func_800ABCC8` (0x800ABF70) ORs 0x400000 into the node flags when the type-table flag halfword (row +0x12)
  has 0x40. **Only T3PYRAMID** (type flags 0x64268: late pass 0x40000, moving 0x8000) has it, so the large pyramid
  stays drawn from any distance instead of being culled with the other far scenery.
- Bit 22 of 0x801174B4 (race HUD flag) is an unrelated word tested with the same constant elsewhere.
- Rush 2 port: stripped because Rush 2's bits 22+ are its own culling index; the pyramid is then culled by Rush 2's
  normal rules.

## 3. Rush 2049 animated objects

### 3.1 PTHD / PATH **[V]**
Track geometry chunk `PTHD` (located by the model loader func_80096CBC only for files 101-119, → 0x801392D0, count
0x801392D4), 36-byte headers; `PATH` holds the nodes they point to.
```
PTHD: char name[16] (type name, §2.1); u32 flags; s16 node_count; s16 trigger; u32 0;
      u32 nodes_offset (file-relative); s32 dynamic_id (-1 none)
PATH node (0x44): f32 pos[3]; f32 dir[3] (unit, towards next); f32 scale[3]; f32 quat[4] (x,y,z,w);
      f32 dist (segment length); f32 time (segment seconds, or pause length); f32 speed (units/s at this node);
      u32 flags
```
PTHD flags (bits seen: 0x1 0x2 0x4 0x8 0x10 0x20 0x40 0x80 0x800 0x1000 0x2000 0x4000 0x8000):
0x1 ping-pong; 0x2 loop (else restart at node 0, func_800C085C); 0x4 present forward / 0x8 present backward
(func_800B2DF8); 0x10 creation parameter [I]; **0x20 carries collision group `trigger`** (its polygons move with it);
0x40 waits for a trigger; 0x80 stops at each end until the next trigger; 0x100/0x200/0x400 runtime
halted/triggered/pending; **0x800 trigger switches collision group `trigger`**; 0x1000 TRIGGER pad;
0x2000 linked trigger [I]; 0x4000 one-shot (reverse and halt at the end); 0x8000 silent.
Node flags: 0x1 spawn an object here at race start; 0x2 no translation (rotation only); 0x4+0x10 no rotation update;
0x8 / 0x10000000 constant speed; 0x40 halt here until the next trigger; 0x1000 runtime "spawned";
0x1000000 battle only [I].

Counts (race tracks 1-6): 28, 25, 26, 34, 34, 43 paths; e.g. track 1: TRAPDOOR2 ×2 (trigger groups 8, 13),
TRAPDOORT1 ×2, BKWBARIER, GLASSWALL, TOILET, TROLLEY ×2 (7 nodes), GONDOLA1/2, TRAINORG ×3, ELEVATOR1 ×4, F1FLAG,
F2FLAG, TRIGGER ×6. Track 6 has 23 MINITRAINN carriages on 7 paths. Full lists: self-check output.

### 3.2 Motion code **[V]**
- Spawn (func_800B338C → func_800B2DF8, race setup): for every PTHD present in this direction/mode, every node
  with flag 1 creates an object via func_800ABCC8 (type by PTHD name, follower state from the pool 0x80138880,
  0x18 bytes: +0 PTHD*, +4 f32 t, +0xC s16 node, +0xE u16 dir flags 4/8, +0x10 f32 speed, +0x14 f32 max speed,
  +0x18 previous position). The object is parented to the top-level section whose box contains the spawn point
  (func_800AB53C) and inherits PTHD id.
- Init func_800C1604: matrix from the node quaternion (func_800BFBE8), position = node pos; registers PTHD flag
  0x20 paths in 0x8013C300; flip-book frame counts.
- Update func_800C0AC0 (type update callback, called by func_800BEAA0 every frame):
  1. trigger state machine (flags 0x40/0x200/0x400/0x1000/0x2000/0x4000, sounds 0x80142A78/7A);
  2. `t += dt` (0x8002EB94); while `t ≥ node.time`: `t -= time`, step to the next node in the travel direction; at the
     ends apply ping-pong (0x1), halt (0x4000, 0x80), restart at node 0 (func_800C085C) or loop (0x2); a node with
     flag 0x40 halts; at the end of a 0x20 path func_800B2D20 restores the collision group;
  3. position (func_800C04CC, unless node flag 2): `pos = node.pos + node.dir * s`, `s = dist * t/time` for
     constant-speed nodes, else `s = v0*t + (v1-v0)*t²/(2*time)` (linear acceleration between node speeds),
     quantised to 1/32; velocity = (pos - previous)/dt;
  4. rotation (func_800C00E0 → func_800BFD8C quaternion interpolation → func_800BFBE8 matrix) unless node flags
     4 and 0x10; scale = lerp(node.scale, next.scale, t/time) multiplied into the matrix rows;
  5. kind 4: func_800AB750 copies matrix, position and velocity (node.dir × speed) into the moving-body table;
  6. PTHD 0x20: func_800BF838 re-transforms the group's collision polygons by (current pose × inverse spawn pose)
     and writes them back (9 s16 per polygon, scale 16384, plus flags).
- Flip-book types advance a model frame counter (+0x58/+0x5A) instead of moving.

### 3.3 Effect on cars **[V]**
- **Moving bodies hit cars.** Bodies with type flag 0x8000 own a 64-byte entry in *0x80150F38 (count 0x80150F78):
  +0 object, +4 matrix, +0x28 position, +0x34 velocity. The car-vs-body test at 0x8010C02C: bounding-sphere check
  (object radius + car radius +0x654), then each of the car's 4 corner points (+0xF4) is transformed into the body's
  local frame (func_800A61B0) and tested against the model's box; a hit calls func_800FD9F8 (collision response with
  the body's velocity). func_800BEE2C uses the same table for a "car inside/under the body" factor (0.75-1.0) [I].
- **Collision polygons move or switch.** Collision files have a dynamic-group table (func_800AB638: header u16
  counts → 0x80124EEC, polygons 0x801497F8 (0x18 bytes), 0x8015201C (8 bytes), **groups 0x801525EC** (0x20 bytes:
  u16 group id, u16 polygon index, 0x12 bytes saved polygon data, ..., count 0x8015267C)). PTHD 0x20 paths move
  their group's polygons every frame (elevators, platforms, ramps, rotors, teeth, pyramid, BKWBARIER, GLASSWALL,
  TOILET...). PTHD 0x800 paths switch them: func_800B2CB4 sets polygon word 0 to 0xF (non-solid, trap door open),
  func_800B2D20 restores the saved data (closed).
- **Triggers are collision polygons.** func at 0x8010C2E4: a wheel's current surface polygon (player +0x5A0..+0x5A6)
  with flag 0x20 and (flags & 0xF800) == PTHD.trigger << 11 triggers that path (TRIGGER pads, trap doors).

So trap doors open a hole in the collision (cars fall through), trains/trolleys/gondolas/planes/boulders/pistons
push cars, and elevators/ramps carry cars on moving polygons.

---------------------------------------------------------------------------------------------------------------------

## 4. Mapping 2049 objects to Rush 2

| 2049 | Rush 2 | Notes |
|---|---|---|
| static sections `TRACKnLxxxxx`, sky, G1 props | plain records (type 0) | lossless; names must exist in the converted geometry |
| CONE1 GASPUMP RAT RATCONE (kind 2), BUMPHIT CURVEHIT METER MPH45HIT MPH75HIT NOPARK SLOWHIT STOPHIT THINKHIT GETOFF (kind 0, sub 1-2), CACTUS | `X49<model>` records (2049 model, renamed so Rush 2's prefix classifier doesn't take CONE1G1 / STOPHITG1 for its own breakables) | prop records; 2049's reactions run in src/track2049_props.cpp (§7) |
| YIELDHIT | YIELDHIT | Rush 2 sign (none in the race tracks; in 2049 the type has no model and is refused) |
| SHATPANE | SHATPANE | Rush 2 glass (SHATPANEO1-7 in 0x14) |
| FLAG2_* | FLAG2* | Rush 2 flag (FLAG2O1 in 0x14) |
| COLLISION | COLLISION | same meaning (collision cylinder from the record) [I for 2049 side] |
| TROLLEY2, WINDMILL(2/3), all path objects | records named after the 2049 model | animated by src/track2049_movers.cpp |
| FENCE | static FENCEG1 record | none in the race tracks or stunt arenas |
| GOLDCOIN, SILVERCOIN | KEYG0-7, KEYS0-7 | Rush 2 key records (behaviour 8), numbered per kind in record order; drawn with 2049's coin models (file 68, merged into the geometry with behaviour 8) and kept per profile by src/collectibles.cpp |
| WEPICON_*, WPR_MINE | dropped | battle only |
| BULB, GUARDRAIL | dropped | no model, type flags 0x60004: func_800ABCC8 refuses `(low flags & ~4) == 0`, so they do nothing in 2049 either (editor helpers; GUARDRAIL_FW x6 on track 2, BULB in no race track) |
| GDAT/GTLD, dynamic ids | dropped | Rush 2 builds its own breakable list |
| Rush 2-only | — | no ambient sound emitters exist in 2049 placements (could be added by hand) |

---------------------------------------------------------------------------------------------------------------------

## 5. Converter (`placement.convert`)

`convert(placement_2049, geometry_2049, prefix, types)` → Rush 2 placement file:
1. Parse WHDR/WOBJ; classify each record with the 2049 type table read from the ROM.
2. Keep static records; map breakable/sign classes per §4 (direction suffix `_FW/_BW` → `_F/_B`); replace other
   dynamic objects by their 2049 model name; drop the rest; promote children of dropped records.
3. Add every PTHD object at its spawn node(s) as a static child of the section containing it (quaternion → matrix).
4. Children that become plain models are made parent-relative (Rush 2 only does that itself for breakables).
5. Strip flag bits ≥ 0x80000; keep the bbox only on top-level records; drop the +0x4C id.
6. Re-emit as a pre-order tree, tree name = **the replaced slot's prefix**; check every name against Rush 2's own
   classifier (no accidental prefix hits) and the sibling rule of §1.1 step 7; optional `finish=` adds the
   `<prefix>FINISH` record. `convert_ex` also returns `report['requires']` (models the geometry converter must
   provide) and `report['animated']` (everything that needs runtime code, with path flags and trigger ids).

Self-check results: all 12 Rush 2 files and all 19 2049 files parse; 0 unresolved names in either game; the six race
tracks convert to 147-313 records (e.g. TRACK1 → 167 records: 117 sections, 17 Rush 2 breakables/signs,
33 static stand-ins, 16 coins, 29 animated objects).

**Lossless:** section placement, matrices, positions, culling boxes, late-pass flag, tree structure, direction
variants, collision volumes. **Lost or approximated:** battle items, flag bit 0x400000 (T3PYRAMID only, see 2.2), GDAT/GTLD, every motion (static at the spawn pose),
moving/switching collision, triggers, flip-book animation, 2049 object sounds.

---------------------------------------------------------------------------------------------------------------------

## 6. What Rush 2 needs for full fidelity

### 6.1 Data (all from the user's 2049 ROM, converted at load time)
- Geometry: 2049 track models + files 82-87 + 78 (F1FLAG frames, TRIGGEROFF/ON) merged into the Rush 2 geometry,
  plus `<prefix>FINISH` (G_DL to the CHKPNT texture-load list) and textures `CHKPNT`/`FINISH`; for slot 0 also `SKYO1`.
- Placement: `convert()` output with the slot prefix.
- Mover table: PTHD/PATH (copied verbatim, `parse_paths`), the type-table rows used (model, kind, flags 0x8000,
  flip-book counts) and, from the collision converter, the dynamic polygon groups and trigger tags (§3.3).
- Slot hygiene: 0x800C5D2C[slot] = 0; avoid slots 1/3 (subway) or disable func_800A4210 for them.

### 6.2 Runtime design (C++ "Track2049Movers", behind the 2049 track option)
1. **Init** (hook after the placement walk in func_800A5110, e.g. at 0x800A570C before func_800814B0): for each
   PTHD spawn create one Rush 2 scene node: `handle = func_8005BE3C(model,0,slots-1,1)` (check ≠ -1),
   `node = func_8007F6DC(handle, matrixPtr, 1, sectionNode, 0x40)` where matrixPtr points to 12 floats owned by the
   mover in spare RDRAM (e.g. above the wings area, < 16 MB). Do **not** emit the static stand-ins for paths in the
   placement in this mode (`static_paths=False`). Keep a follower struct per mover (2049 layout of §3.2).
2. **Tick** (hook in func_80076578 next to the subway update func_80075FB8, same dt as the physics): run the 2049
   algorithm of §3.2 exactly (segment stepping, accel profile, slerp, scale lerp, end behaviours, triggers) and write
   m[9] + pos[3] into the node's floats, made relative to the parent section (sections are unrotated). Keep velocity.
3. **Car interaction:**
   - moving bodies (type flag 0x8000): either reuse the subway trick (allocate pseudo-car 0x81C structs and keep
     their position/velocity/orientation in sync) or port 0x8010C02C: sphere test, 4 car corners into the body frame,
     box test, then push the car with the body's velocity through Rush 2's crash response;
   - moving collision groups (PTHD 0x20): transform the group's polygons in Rush 2's collision data each tick
     (needs the collision converter to keep groups, and cells large enough to contain the swept polygons);
   - switched groups (PTHD 0x800, trap doors): toggle the polygons' solidity when triggered;
   - triggers: if Rush 2 exposes the wheel surface polygon, test its tag; otherwise test car position against the
     TRIGGER pad's box (approximation).
4. **Drawing:** automatic through func_8007B518. Give each mover a stable RT64 matrix group id so frame
   interpolation does not smear them; restore lighting state as noted for the wings if any custom DL is used.
5. **Modes:** honour PTHD 0x4/0x8 against Rush 2's backward flag 0x80119848; in 2-4 player games 2049 still runs
   movers (only Rush 2 breakables are skipped).
6. **In-place animations** (WINDMILL, WINDMILL2, TROLLEY2, flip-books F1FLAG/SHARK): rotate about the local axis or
   swap the node's model handle (+0xC) on a timer.

---------------------------------------------------------------------------------------------------------------------

## 7. Knock-over props **[V]**

Port: src/track2049_props.cpp. Records: the converter's `PropRecord` list (record index, type row, direction, world
pose and the parent's pose).

**Hit test.** Type flags high s16 = car callback slot; props use slot 0 = 0x8010C6C8. func_800BEAA0 (per car,
per frame) walks the GDAT leaf around the car and, for objects with flags 2 (armed) and 8 (car callbacks), calls
`slot(&car, obj+0x44, obj+0x54, 0)`: hit when `|car pos (0x80152818 + 0x3B8*car, +8) - obj+0x44|^2 <= (r + 3.5)^2`
and the car is active (+0x7EA). obj+0x44 = the node's world position (func_800ABBD0), obj+0x54 = type +0x18 or, if
that is -1, the model's OBHD radius (func_800ABB58). On a hit: obj flags |= 4, obj+0x5C = car, type init (+8) runs;
the init clears flags 2 and 4 so it hits once. Props never push the car.

**Reactions** (all per rendered frame; car velocity = 0x80152818 struct +0x14):
- Kind 2 (CONE1, GASPUMP, RAT, RATCONE), func_8010DCFC: pooled block obj+0x6C = {spin[3], vel[3]} (pool 0x80150E98,
  24 bytes). vel = car vel x 0.125; obj matrix = func_8008B4C4(vel) (z row = vel normalised, x row = (vz, 0, -vx)
  normalised or (1,0,0) if its length <= 0.01, y = z x x, x = y x z); vel.y += 2, or 1 for models 0xED RATG1 and
  0x153 RATCONEG1; spin = (0, 12, 15); sound type +0x1C via func_800FEA00(id, car, pos, 2); instance timer 5.0.
  Update func_8010E4E4: `vel += g x 0.15; pos += vel + g x 0.15` with g = 0x80121DDC = (0, -0.25, 0), then
  func_800D03DC(spin x dt) (rotate y, then x, then z, in the object's frame); timer -= dt; at 0: type flag 0x2000
  (CONE1, GASPUMP) -> node removed (func_80090088); the object is freed. No ground collision.
  (D_8013FECC / D_8013FECD branches for model 0xEC, the cone, are a 2049-only mode and not ported.)
- Kind 0 sub 1-2 signs, func_8010E72C: obj matrix = func_8008B4C4(car vel); obj+0x5A = 4; update func_8010E828
  rotates by the sub-kind's angles (kind table 0x80118DDC[0] = 0x80118D70, 12-byte entries: sub 1 (-0.38397, 0, 0),
  sub 2 (-0.37525, 0, 0)) once per frame for 4 frames; the sign stays down.
- CACTUS, func_8010D9CC: obj+0x5A = 7; obj matrix = func_8008B4C4(car vel x 0.125); instance timer 1/30 (0x801249BC);
  update func_80094888 (generic flip-book): timer -= dt; at <= 0: frame++, timer = 1/16; at frame >= 7: type flag
  0x1000 -> stop on the last frame (CACTUS), else 0x2000 -> remove, else loop; model = handle table entry
  (obj+0x58 anim base + frame): CACTUS 263 = CACTUSG2..G8 (names 0x8011AD68).

The port scales the per-frame steps by the tick length in 30 Hz frames.
