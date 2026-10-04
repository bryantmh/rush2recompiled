# Rush 2049 moving track objects: specification for the Rush 2 port

Tags: **[V]** verified in disassembly or data; **[I]** inferred. Addresses are 2049 main (`tools/rush2049/out/d49m.asm`,
the lui-annotated copy of 2049 main) unless marked "R2" (Rush 2, `analysis/out_disasm/r2.asm`). Formats of PTHD/PATH,
the type table and the collision MOVER section are in placement.md §2–3 and collision.md §3; this file corrects
them where noted.

Port: `include/track2049_movers_logic.h`, `src/track2049_movers_logic.cpp` (pure C++, namespace
`rush2::track2049::movers`). Test: `tools/rush2049/cpp_test/movers_build.bat [seconds] [track]` (runs race tracks
1–6 both directions, fires the pads, and checks that the spawn pose's collision rewrite gives back every MOVER rest
record: it does on all 12 track/direction pairs, within 1/16384 and 1/32).

Matrices are 3×3 row-major float[9]; points are row vectors (`p' = p·M`, R2 func_8006929C / 2049 func_8009E820).
All maths is single precision, in the operation order given (the C++ keeps it; FP contraction is off).

---------------------------------------------------------------------------------------------------------------------

## 1. Globals and timing

| Address | Meaning |
|---|---|
| 0x8002EB94 | f32 frame time (variable, per rendered frame) [V] |
| 0x801392D0 / D4 | PTHD array / count (file-relative +0x18 and +0x1C relocated at load by func_80096CBC area, 0x800970C0) [V] |
| 0x80138880 | follower pool (0x48-byte entries: size table 0x80117510[4] = 72) [V] |
| 0x801391F0 | instance (process) list; +0x0 next, +0x4 s16 frame, +0xC object, +0x10 f32 timer, +0x14 update fn [V] |
| 0x80143FC8 | object pool; +0x10 active list (push-front), +0x14 free list (func_8008E3C0) [V] |
| 0x80150F38 / F78 / F80 | moving-body table ptr / capacity / used [V] |
| 0x8013C300 / 0x8013F1DC | objects with PTHD flag 0x20 (platform lookup list) / count [V] |
| 0x80152570 | backward flag [V] |
| 0x80156994 | 1 when osMemSize > 4 MB (Expansion Pak), set at 0x800E7DAC [V] |
| 0x801174B4 | game state bits; update needs `& 0x7C0000` or `& 8`; `& 8` without Expansion Pak = no movers [V code, I meaning: 8 = multiplayer] |
| 0x801170FC | paused: updates return immediately [V] |
| 0x8014A110 | game mode; 2 = battle-only nodes/groups [I], 5 = group moves only within 150/450 ft of car 0 [V code]; race uses neither [I] |

**Frame order [V]:** frame function func_800FD464 → func_800FBF88 (game step: func_800EC0DC car snapshot
extrapolation, func_800FA9B4 per-car loop → func_800BEAA0 car-vs-object callbacks) → func_800F733C → **func_800B0868**
(walks 0x801391F0 calling each instance's update with a1 = 1) → … So **followers update once per rendered frame with
the frame dt, after the car/object tests**: bodies and triggers are tested against the previous frame's pose. Car
physics runs in its own thread (func_800E7710 → func_800E6AF8) at fixed 1/60 s ticks.

---------------------------------------------------------------------------------------------------------------------

## 2. Data (corrections to placement.md)

- PTHD +0x18 is a **link**: file offset of another PTHD (relocated to a pointer; 0 = none, relocated to the file
  base and never used). TRIGGER pads link to the object they open. +0x20 is overwritten at runtime with the follower
  pointer by func_800C085C when flags & 0xC0. [V]
- PTHD +0x10 flag 0x8000 = **no instance model writes** (TRIGGERON/OFF swap, flip-book) — not "silent"; the start/stop
  sounds still play [V]. 0x10 = clear object flag 8 (no car callbacks: no body collision, no trigger test) [V].
- Type table +0x10: high s16 = **car callback slot** (table 0x80117518: 4 = moving body 0x8010C02C, 5 = trigger test
  0x8010C2E4; 6 = no flag 8), low u16 = flags (0x20 spawn without Expansion Pak, 0x4000 init via func_800AB7D8, 0x8
  pooled, 0x8000 counts a body). The "type flag 0x8000" bodies are exactly slot 4. [V]
- Type +0x18 (f32) for slot 4 = body box class (0x80118C10 + 32·class, §6.1). +0x1C/+0x20/+0x24 = start/loop/stop
  sound, +0x28 sound flags, +0x2C range. [V]
- Instance model handles: 0x80142A78 = **TRIGGERON**, 0x80142A7A = **TRIGGEROFF** (handle table 0x801427C0, names
  0x8011AD68+4i) [V]. Flip-book bases 0x169 = SHARK (SHARKG2/G4/G5/G16), 0x16D = F1FLAG (F1FLAGG28..G47) [V].
- Path objects are **not parented**: func_800B2DF8 passes parent −1 to func_800ABCC8; positions are world. [V]

---------------------------------------------------------------------------------------------------------------------

## 3. Spawn (func_800B338C → func_800B2DF8 → func_800ABCC8 / func_800AB7D8 → func_800C1604) [V]

func_800B338C resolves the shared model handles (incl. TRIGGERON/OFF, flip-book frames), then func_800B2DF8:

```
if ((0x801174B4 & 8) && !expansion) return;                      // also func_800ABCC8 refuses
for k, p in PTHD:
  if (mode == 2 && p.group > 0 && no node has 0x01000000) { disable(p.group); continue; }
  present = (f&4 && f&8) || (backward && f&8) || (!backward && f&4)
  if present:
    for node in p.nodes: (restart: node.flags &= ~0x1000); if (node.flags & 1) func_800ABCC8(p, parent -1, …, path k)
    if (restart && p.group > 0) restore(p.group)                   // 0x80150DD0 = restart [I]
  else if (f & 0x800) disable(p.group)                             // func_800B2CB4
```
func_800ABCC8: type = first table row whose name prefixes p.name; refuse if `!expansion && !(low & 0x20)` or
`(low & ~4) == 0`; object +0x65 = path index, +0x50 = +0x58 = type anim, +0x5A = 0, +0x60 = −1, obj+4 = 2 (low & 4 →
0), |= 8 unless slot 6. Objects go to the front of the active list.

func_800AB7D8 visits the active list (so **reverse creation order**): follower = pool entry with +0 = PTHD[obj+0x65];
PTHD 0x10 clears obj flag 8; slot 4 → obj+0x65 = next body index, body entry box = class box; calls type init
func_800C1604, which:
```
obj+0x64 = 2 (sound stopped); f.dir = 0; f.t = 0
if (p.flags & 0x20) push obj on 0x8013C300
f.max_speed = max(0, node speeds)
first node with (flags & 1) && !(flags & 0x1000): restart(node); node.flags |= 0x1000
anim 0x169: count 4, timer 0.25; 0x16D: count 20, timer 0.0425 (0x80123E90)
if ((p.flags & 0x40) && !(p.flags & 0x4000)) { p.flags = p.flags & ~0x100000 | 0x200400; model TRIGGEROFF unless 0x8000 }
instance (update fn = type +0xC) pushed on the front of 0x801391F0
```
Net effect: update order = creation order (path order, node order); within a path the **last-created object takes
the first spawn node**. Single vs multi player: nothing path-specific beyond the 0x801174B4/Expansion test (Rush 2 has
8 MB: treat as expansion = true). Mode-2 (battle) branch irrelevant to races.

**restart(n) = func_800C085C [V]:**
```
f.dir = (p.flags & 1 && n >= count-1) ? 8 : 4; f.node = n; f.t = 0; f.speed = 0
obj.pos = f.prev = node[n].pos
if (p.flags & 0xC0) { p.flags |= 0x100; p.follower = f }
if (p.flags & 0x5000) p.flags |= 0x400
obj.m = quat_to_matrix(node[n].quat); f.inv_rest = transpose(obj.m)    // func_8008D6B0 + func_800C0828
obj.m[r][c] *= node[n].scale[r]
```
Note: a restart from the end of the path recomputes `inv_rest` from node 0, not the spawn node.

---------------------------------------------------------------------------------------------------------------------

## 4. Per-frame update func_800C0AC0(instance, a1) [V]

`a1 == 0` destroys. Returns unless `(0x801174B4 & 0x7C0000 || & 8) && !paused`. `p` = PTHD, `f` = follower, `N[i]` =
node, `tt = (f.dir & 8) ? N[f.node].time - f.t : f.t` everywhere.

### 4.1 Trigger state machine (only if p.flags & 0x40)
```
a = p.flags
if (a & 0x4000) {                                    // one-shot: runs while held, then returns
  if (!(a & 0x200)) {
    if (!(a & 0x400)) { if (f.dir & 4) { f.dir = f.dir&~4|8; f.t = N[f.node].time - f.t; }
                        if (p.flags & 0x100) sound_start(); p.flags &= ~0x100; } }
  else { if (a & 0x400) { p.flags = a & ~0x500; sound_start(); } p.flags &= ~0x200; }
} else {
  L = p.link
  if ((a & 0x2000) && (a & 0x100000) && (L.flags & 0x100)) {            // linked pad: target stopped -> re-arm
    p.flags = a & ~0x100000 | 0x200400; model TRIGGEROFF; sound_stop(); return; }
  if ((a & 0x400) && (a & 0x200)) {                                     // armed and triggered
    if (L.flags & 0x100) L.flags &= ~0x100;                              // start the target
    else if (a & 0x1000) { g = L.follower; g.dir ^= (4|8); g.t = L.N[g.node].time - g.t; }   // reverse it
    a = p.flags & ~0x700; p.flags = a
    if (a & 0x1000) { if (a & 0x200000) { p.flags = a&~0x200000|0x100000; model TRIGGERON; sound_start(); }
                      else              { p.flags = a&~0x100000|0x200000; model TRIGGEROFF; sound_stop(); } }
    else if (a & 0x200000)              { p.flags = a&~0x200000|0x100000; model TRIGGERON; sound_start(); }
  }
}
if (p.flags & 0x100) return;                         // halted: no motion, flip-book, body or scale update
```
Pads re-arm: 0x1000 pads via their own 2-node path ending (restart sets 0x400 again, ~0.2 s later); 0x2000 pads
only when their target halts. A stale 0x200 stays set until the pad is armed again.

### 4.2 Stepping
```
f.t = f.t + dt
loop while N[f.node].time <= f.t:
  f.t = f.t - N[f.node].time
  if (f.dir & 8):
    if (f.node == 0):
      if (p.flags & 0x4000) { restart(0); sound_stop(); done }
      else { f.dir = 4; if (p.flags & 0x80) { p.flags |= 0x100; f.t = 0; done } }
      if (p.flags & 0x20) restore(p.group)                       // func_800B2D20
    else f.node--
  elif (f.node + 1 == count):                                    // loop closing segment or stop-at-ends end
    if (p.flags & 0x80) { f.dir = 8; f.node-- }
    else { f.node = 0; if (p.flags & 0x20) restore(p.group) }
  else:
    f.node++
    if (N[f.node].flags & 0x40) { p.flags |= 0x100; f.t = 0; done }
    elif (f.node + 1 == count):
      if (p.flags & 1)         { f.dir = 8; f.node-- }                         // ping-pong
      elif (p.flags & 0x4000)  { f.dir = 8; f.node--; f.t = 0; p.flags |= 0x100 }
      elif (!(p.flags & 2))    { restart(0); if (p.flags & 0x20) restore(p.group) }
      // loop (2): stay on the last node, whose segment wraps to node 0
```
(`f.dir = 8` means `f.dir = f.dir & ~4 | 8`.) Backward travel on segment n runs from node n+1 to node n.

### 4.3 Pose
```
moved = false
if (!(N[f.node].flags & 2)) { position(); moved = true }
nf = N[f.node].flags
if (!(nf & 0x10) || !(nf & 4)) { moved = true; rotation() }
moved ? (p.flags & 0x100 ? sound_stop() : sound_moving()) : sound_stop()
```
**position() = func_800C04CC:**
```
n = f.node; if (!(n < count-1 || p.flags & 2 || !(p.flags & 0x80))) return      // parked at the end
if (N[n].flags & 0x10000008) s = N[n].dist * (tt / N[n].time)                    // constant speed
else { nx = n+1 (0 if >= count and loop); v0 = N[n].speed; dv = N[nx].speed - v0
       s = v0*tt + ((tt*tt)*dv) / (N[n].time*2.0f) }                            // linear acceleration
off[i] = N[n].dir[i] * s
f.prev = obj.pos
obj.pos[i] = (float)(int)trunc(off[i]*32.0f) * 0.03125f + N[n].pos[i]          // 1/32 truncation of the offset
d = obj.pos - f.prev
f.speed = sqrtf(d2*d2 + (d0*d0 + d1*d1)) / dt
if (p.flags & 0x20) translate(p.group, d)                                        // func_800C0294 (mode 5: distance gate)
```
**rotation() = func_800C00E0:**
```
if (N[n].flags & 0x20 || N[n].time == 0) obj.m = quat_to_matrix(N[n].quat)
else { nx = n+1 (0 if >= count and loop); obj.m = quat_to_matrix(quat_interp(tt/N[n].time, N[n].quat, N[nx].quat)) }
if (p.flags & 0x20) rotate(p.group, obj.pos, f.inv_rest, obj.m)                 // func_800BF838
```
**quat_to_matrix = func_800BFBE8(m, q, 1)** (s = 2, no normalisation; with a2 = 0 it uses s = 2/|q|²):
`xs=x*2…; xx=x*xs, yy=y*ys, zz=z*zs; m0=1-(yy+zz), m4=1-(xx+zz), m8=1-(xx+yy); xy=x*ys, wz=w*zs: m1=-(xy+wz),
m3=-(xy-wz); yz=y*zs, wx=w*xs: m5=-(yz+wx), m7=-(yz-wx); xz=x*zs, wy=w*ys: m2=xz-wy, m6=xz+wy`.

**quat_interp = func_800BFD8C(t, q0, q1, out):** t < 0.001 → q0; t > 0.999 → q1; else
`c = (q1w*q0w + ((x0x1 + y0y1) + z0z1)) * 0.999f`; if c < 0 → r = −q1, c = −c, else r = q1. If c < 0.98:
`a = acos(c); inv = 1/sinf(a); k0 = sinf((1-t)*a)*inv; k1 = sinf(t*a)*inv; out = r*k1 + q0*k0`. Else, per component:
`e = r-q0; if (1<e) e-=2; if (e<-1) e+=2; v = q0 + t*e; if (1<v) v-=2; else if (v<-1) v+=2` (no normalisation).
acos = func_8009C3F8(a0=1) (Cephes-style rational asin, constants 0x80123ABC–AE8, quadrant tables 0x8011F010/18);
sinf/cosf = libultra 0x80008730/0x800088F0 (double-precision polynomial). All ported bit-exact.

### 4.4 Flip-book, body, scale
```
if (type.anim != -1):
  inst.timer = inst.timer - dt; if (!(inst.timer <= 0)) return       // returns: no body/scale update this frame
  inst.timer = 0.25 (SHARK) / 0.0425 (F1FLAG) / unchanged (others)
  inst.frame++ (wrap at obj+0x5A); m = obj+0x58 + frame; if (m != obj+0x50) { instance model = handle[m] unless 0x8000; obj+0x50 = m }
if (slot == 4) body[obj+0x65] = { obj.m, obj.pos, N[n].dir * (f.dir&8 ? -f.speed : f.speed) }   // func_800AB750
if (!(N[n].flags & 0x10)):
  nx = n+1, 0 if >= count (regardless of loop); r = tt / N[n].time
  s[i] = (N[nx].scale[i] - N[n].scale[i]) * r + N[n].scale[i];  obj.m[row i] *= s[i]
if (!(obj+4 & 2)) destroy                                          // never in races [I]
```
Quirk: scale multiplies the matrix even when rotation() did not rebuild it (compounds). 2049 data only scales
nodes that also rotate.

### 4.5 Sounds [V]
func_800BF394 start: stop current, play type +0x1C, state 0. func_800BF1C8 stop: if state ≠ 2 and the start sound is
not still playing: stop, play +0x24, state 2. func_800BF45C moving: state 2 → start; state 0 and start finished →
play loop +0x20, state 1; state 1 → update loop: sound 1 pitch `f = clamp((|(int)speed| / |(int)max_speed|)*0.5+0.5)`,
volume/pitch (0.75f+0.25, f); sound 0x12 (0.8, 1.0); 0x61 (1.0, 0.75). All via func_800AED64 at obj+0x38 with range
type +0x2C. The C++ port reports start/loop/stop requests only.

### 4.6 In-place animations (not path objects) [V]
TROLLEY2 (sub 3), WINDMILL (sub 4), WINDMILL2 (sub 5) are placement objects (kind 0, no init) whose update
func_8010E694 calls func_800D03DC(rate·dt, obj+0x14) every frame (unless paused). Rates (0x80118D70 + 12·sub):
sub 3 (0, 1.5, 0), sub 4 (1.5, 0, 0), sub 5 (−1.5, 0, 0) rad/s. func_800D03DC rotates the matrix about y by a[1]
(func_80090E9C: `m[c] = m[6+c]*s + m[c]*co; m[6+c] = m[6+c]*co - m[c]*s`), then about x by a[0] (func_80090F44:
`m[3+c] = m[3+c]*co - m[6+c]*s; m[6+c] = m[3+c]*s + m[6+c]*co`), then about z by a[2] (func_8009EA68:
`m[c] = m[c]*co - m[3+c]*s; m[3+c] = m[c]*s + m[3+c]*co`); each skipped when |angle| ≤ 0.0001. Port:
`rotate_in_place()`. Knock-over signs (sub 1/2, func_8010E828) only tick a counter.

---------------------------------------------------------------------------------------------------------------------

## 5. Collision groups [V]

MOVER/group table *0x801525EC (0x20 bytes, count u16 0x8015267C): +0 u16 group, +2 u16 poly, +4 s16[9] rest matrix,
+0x16 u16 vertex (the polygon's origin), +0x18 8-byte rest vertex. Only the **origin vertex and the matrix** change;
the other vertices are polygon-local. VERT decode: `((s16 << 5) + 5-bit fraction) * 0.03125f`.

- **translate (func_800C0294, from position()):** per group vertex, from the *current* value:
  `n = delta + v; q = n*32; qi = q<0 ? (int)(q-0.5f) : (int)(q+0.5f)`; s16 = qi >> 5, frac =
  `(qz&0x1F) | ((qx<<10)&0x7C00) | ((qy<<5)&0x3E0)`. Accumulates; a step below 1/64 never moves it.
- **rotate (func_800BF838, from rotation(); overwrites translate for rotating objects):** from the rest copy:
  `R = Rest·inv_rest·obj.m` (func_800BF780 `out = B×A`, element `A[6+j]*B[i][2] + (B[i][0]*A[j] + B[i][1]*A[3+j])`),
  matrix = `(s16)trunc(R*16384.0f)`; vertex `v = pivot + ((rest - pivot)·inv_rest)·obj.m`, `q = trunc(v*32)` (no
  rounding), packed as above. pivot = current object position.
- **restore (func_800B2D20):** POLY+4 ← rest matrix (0x12 bytes), VERT ← rest vertex. Does **not** touch POLY flags.
- **disable (func_800B2CB4):** POLY u16 flags = 0x000F (type 0xF, material cleared). Only used at spawn, for
  0x800 paths absent in this direction/mode — permanent.

Port: `World::take_group_ops()` yields {translate delta | rotate pivot, inv_rest, m | restore | disable} in 2049's
order; `apply_translate()` / `apply_rotate()` reproduce the arithmetic on raw POLY/VERT data.

---------------------------------------------------------------------------------------------------------------------

## 6. Effects on cars [V unless marked]

"Car" = 2049 player struct 0x8014A250 + i·0x808. `MUL(v,M)` = world→local (`o[i] = M[i][2]*v2 + (v0*M[i][0] +
v1*M[i][1])`, func_800A61B0); `TMUL` = local→world (func_8009E820); `LEN` = `sqrtf(z²+(x²+y²))`.

### 6.1 Dispatch
func_800BEAA0(car) per car per rendered frame (from func_800FA9B4 at 0x800FACD8 in the game step; skipped like the
spawn when `0x801174B4 & 8` without Expansion Pak): finds the GDAT leaf containing the car (car-state +0x8/+0x10 x/z),
and for each object of that leaf with obj+4 bits 2 and 8 switches on the type slot (jump table 0x80123E5C); slots 3–5
are called `(obj, &carIdx, 0, 0)`. **A body only hits a car whose GDAT leaf lists it.** Body boxes
(0x80118C10 + 32·class: xmin, xmax, zmax, zmin, ymax, ymin, radius, mass):

| class | type | x | z | y | radius | mass |
|---|---|---|---|---|---|---|
|0|GONDOLA1|−7..7|−30..30|−14.6..0|45|4000|
|1|GONDOLA2|−7..7|−33..33|−14.6..0|45|4000|
|2|ELEVATOR1|−4..4.5|−10..10|0..28|45|4000|
|3|TRAINORG|−10..8|−144..142|0..12|155|5000|
|4|TROLLEY(NT)|−6.5..6.5|−18.5..17.7|0..18.5|40|4000|
|5|MINITRAIN(N)|−5.4..5.4|−22..22|0..14.5|40|4000|
|6|PLANE|−20..20|−30..30|−10..10|70|6000|
|7|PISTON|−5..5|−10..10|0..15|50|6000|
|8|TEETH|−10..10|−5..5|0..10|30|6000|
|9|BOULDER|−40..40|−40..40|−40..40|120|6000|
|10|BLOCKNV|−18..18|−10..10|0..39|120|6000|

Many race bodies have PTHD 0x10 (ELEVATOR1, GONDOLA2, some TRAINORG/GONDOLA1 paths) and therefore **never collide**
(`Object::cars == false` in the port).

### 6.2 Moving body vs car: 0x8010C02C(obj, &idx) and func_800FD9F8
```
e = body[obj+0x65]; B = e.box; if (c.s8_7EA == 0) return
d = c.pos794 - e.pos; s = ((0 + d0²) + d1²) + d2²; R = B.radius + c.f654; if (R*R < s) return
for corner in c.corner[4] (+0xF4, car-local):
  w = TMUL(corner, c.m7A0) + c.pos794 - e.pos; p = MUL(w, e.m)
  if (p inside [xmin,xmax]×[ymin,ymax]×[zmin,zmax]) { FD9F8(c, e, d, p); return }   // first corner only
FD9F8: L = MUL(c.pos794 - e.pos, e.m); inZ = zmin < L2 < zmax; inX = xmin < L0 < xmax
  if (inZ && inX && c.s8_640) { push(c, d); return }       // FD8DC: wrecked inside: 100000 along d (car-local add)
  RV = MUL(c.vel788 - e.vel, e.m)
  fz = (inZ && !inX) ? 0 : RV2*2500; p2>0: fz = min(fz, -4000), inX: t=(p2-zmax)*(10000*|p2-zmax|), fz = min(fz,t);
                                     p2<0: fz = max(fz, 4000),  inX: t=(p2-zmin)*(10000*|…|),      fz = max(fz,t)
  fx likewise with RV0, xmax/xmin, gain 20000, zeroed when (inX && !inZ)
  fy = RV1*100
  C = MUL(TMUL((fx,fy,fz), e.m), c.m7A0) * (B.mass / ((c.f5C4 + B.mass)*0.5))
  c.f124..12C -= C                                          // car-local external force
  if ((opt 0x8017A634 == 1 || (opt == 2 && c.s8_7CC == 2) || c.f63C*0.8 < LEN(C)) && mode != 6
      && !c.s8_640 && carstate.s8_359 != 2) c.s8_640 = 1    // wreck
```
No damage, sound or explicit torque; constants 0x80124808–20. The +0x124 accumulator is added into the body force
by func_800E1C30 and cleared at the end of every physics step (func_800E3724, 0x800E393C): **a body's force, written
once per rendered frame, acts for exactly one 1/60 s tick**.

func_800BEE2C(car, &factor) (AI only, from func_800E398C at 0x800E3F0C): for bodies 0..n−2 (off-by-one skips the
last), `L = MUL(e.pos - car.pos794, car.m7A0)`; if `0 ≤ L2 ≤ 225 && |L0| ≤ 14`: `f = L2 < 125 ? 0.75 : 1 - (225 -
L2)*0.0025`; `factor = min(factor, f)` (AI obstacle-ahead speed factor [I]).

### 6.3 Ride-on platforms (POLY info 0x20) and boost pads (0x10)
Wheel probe func_800C6AA0 (0x800C6DE0) records `c.poly648` = the polygon if found, the wheel is within 1.0 and
`info & 0x30`; cleared in func_800CF06C. func_800E1F80(c) runs from the drag function func_800E23A4 (0x800E2700)
each physics step when set (order in func_800E2A64: probes/car-car → drag + E1F80 → force sum):
```
info = P.info; id = info >> 11; spd = id
if (info & 0x10) { spd = id*8                                      // mph
  if (spd >= 51) { a = P.m[0..2]*2^-14;                           // strong mode (data: 88..248 mph)
     v = a2*c.vel228 + (a0*c.vel220 + a1*c.vel224); k = (spd*1.4666667f - v) * c.f5C0
     c.f124.. += MUL(a*k, c.m2EC); return } }
n = wheels w with c.f5EC[w] <= 0 && POLY[c.u16_5A0[w]].info & 0x30
if (n >= 3) {
  if (info & 0x20) { v = C1A00(id); c.pos22C += v0*c.f634; c.pos234 += v2*c.f634 }       // carried, x/z only
  else { t = (spd*1.4666667f)*c.f634; c.pos22C += (t*P.m[0])*2^-14; c.pos234 += (t*P.m[2])*2^-14 }
  return }
for each such wheel: v = (info & 0x20) ? normalize(C1A00(id)) : P.m[0..2]*2^-14; v *= 30*c.f5C0
  c.wheelF[w] (+0x64+12w) += MUL(v, c.m2EC)
```
func_800C1A00(id): first object in 0x8013C300 whose PTHD group == id → `N[f.node].dir * (f.dir & 8 ? -f.speed :
f.speed)` (same formula as the body velocity of §4.4, read on demand; the list is in init order, i.e. reverse
creation order). Port: `World::platform_velocity(group)`.

### 6.4 Triggers: 0x8010C2E4(obj, &idx) (slot 5: TRIGGER, T3POD, T3TEETER)
```
if (c.s8_640 || c.s16_6C4 >= 0) return                              // wrecked / not driving
key = p.group << 11
hit = any wheel w: I = POLY[c.u16_5A0[w]].info; (I & 0x20) && (I & 0xF800) == key   // last polygon, even airborne
if (hit) { if ((p.flags >> 24) == 0 || p.flags & 0x4000) p.flags |= 0x200; p.flags |= 1 << (24+idx) }
else p.flags &= ~(1 << (24+idx))
```
Edge-triggered by the occupancy bits 24–31, except one-shot paths which re-trigger every frame while occupied.
Port: `World::car_on_group(car, group, on)`, `World::fire(group)`.

---------------------------------------------------------------------------------------------------------------------

## 7. 2049 → Rush 2 car field map

| meaning | 2049 player (+i·0x808) | Rush 2 car (0x800F5470+i·0x81C) | tag |
|---|---|---|---|
| body-frame force sum | +0x10/14/18 | +0x04/08/0C | [V] func_800E1C30 ↔ R2 func_8006A02C |
| external force accumulator (local, cleared each step) | +0x124/128/12C | +0x11C/120/124 (cleared R2 0x80071F7C, added 0x8006A204) | [V] |
| per-wheel force | +0x64+12w | +0x58+12w | [V] |
| angular velocity (body) | +0x4C..54 | +0x40..48 | [V] |
| world velocity | +0x220..228 | +0x218..220 | [V] |
| world position | +0x22C..234 | +0x224..22C | [V] |
| physics matrix | +0x2EC | +0x2CC | [V] |
| snapshot time / scale | +0x714 / +0x7F4 | +0x714 / +0x818 | [V] |
| snapshot vel / pos / matrix | +0x788 / +0x794 / +0x7A0 | +0x7A4 / +0x7B0 / +0x7BC | [V] |
| corner points ×4 (local) | +0xF4 | +0xE8 | [V] R2 func_8006EC78 |
| radius | +0x654 | +0x658 | [V] |
| mass | +0x5C0 | +0x5B0 | [I] |
| collision mass | +0x5C4 | +0x5B4 | [V] R2 func_8006E2A8 |
| wreck force threshold | +0x63C | +0x644 | [V] |
| wrecked (s8) | +0x640 | +0x648 | [V] |
| step dt / 1/dt | +0x634 / +0x638 | +0x638 / +0x63C | [V] |
| wheel height (≤0 contact) | +0x5EC+4w | +0x5DC+4w | [V] |
| wheel polygon index (u16) | +0x5A0+2w | +0x594+2w (stored at R2 0x8006F964) | [V] |
| wheel info copy | masked to 0x7FF | +0x61C+2w | [V] |
| contact polygon (0x30) | +0x648 | none | [V] |
| index / active / drone | +0x7C6 / +0x7C8 / +0x7CA | +0x7E0 / +0x7E4 / +0x7E6 | [V] |
| collidable | s8 +0x7EA | s16 +0x804 | [I] |
| speed | +0x3F0 | +0x3D4 | [V] |
| +0x6C4 (< 0 to trigger) | +0x6C4 | unknown | [I] |
| car-state dead/out (≥2) | 0x80152818+i·0x3B8 +0x359 | 0x801124A0+i·0x354 +0x343 | [V] |
| wreck-on-contact option | s8 0x8017A634 | s8 0x80119628 | [V] |

Physics function pairs: E2A64 ↔ R2 800706D0, CF06C ↔ 800703EC, E23A4 ↔ 8006AFD8, E1C30 ↔ 8006A02C,
C6AA0 ↔ 8006F704 [V by fmatch + code].

---------------------------------------------------------------------------------------------------------------------

## 8. Rush 2 hook design (recommended)

Rush 2 physics thread: func_80076DE0 → **func_80076578(dt)** per tick (dt = 1/60 NTSC, 1/50 PAL, 0x800CF968/96C):
time += dt → func_800763FC → **func_80075FB8 (subways)** → func_80075DC0 (humans → 80075880 → 80071D78) → 80075C3C
(drones) → 8006912C (snapshots) → 8006890C; it runs only in race states [V].

1. **Spawn:** after the placement walk in R2 func_800A5110 (plan §6.2: e.g. 0x800A570C): `parse_paths` on the 2049
   geometry, `parse_types` on 2049 main, `World::init({backward = 0x80119848})`; create one scene node per object
   (`func_8005BE3C(model)`, `func_8007F6DC(handle, matrixPtr, 1, -1, 0x40)` with 12 floats owned in spare RDRAM);
   apply init's disable ops; keep per-group MOVER rest copies from the converter (polygon/vertex indices must be kept
   1:1 by the collision converter).
2. **Follower update:** hook `before_vram 0x800765F4` in func_80076578 (after func_80075FB8 returns; dt =
   `MEM_F(sp+0x18)`), call `World::update(dt)` per tick. 2049 updates per rendered frame after the car tests; per
   tick is the closed-form same path, only the speed estimate and func_800C0294's accumulation granularity differ.
   For exactness of motion one could instead accumulate ticks and update once per Rush 2 game frame; per tick keeps
   collision in step with physics and is recommended. Then write `m` and `pos` into the node's 12 floats (world;
   nodes are unparented), the flip-book model handle into node +0xC, and TRIGGERON/OFF likewise.
3. **Collision groups:** apply each GroupOp to Rush 2's in-RAM POLY (0x8011001C) / VERT (0x80110050) with
   `apply_translate`/`apply_rotate`/rest copy. Rush 2 has no type 0xF: for `disable`, move the origin vertex far below
   the world or use a type Rush 2's wheel and body probes both ignore [I]. Quadtree leaves must already list the
   polygon in every cell it sweeps (converter's job).
4. **Moving bodies:** port 0x8010C02C/func_800FD9F8/func_800FD8DC on the Rush 2 fields of §7, at
   `before_vram 0x800706E4` in func_800706D0 (`$s0` = car; after probes and car-car, before drag and force sum),
   writing +0x11C..124 with the same sign, using the snapshot pos/vel/matrix +0x7B0/+0x7A4/+0x7BC. Test only
   objects with `cars && body`; GDAT leaf filtering can be replaced by the sphere test. 2049 applies the force for one
   tick per rendered frame (≈ every 2nd–3rd tick at 2049's 20–30 fps): gate the test to once per 2 ticks (or scale the
   force by 1/2) [I]. The subway pseudo-car route (R2 func_8006F3C0 → func_8006EC78 → func_8006E2A8) is a different
   response (gain 2000, no z depth term, ×0.5 wreck threshold, damage/sound, reaction on the pseudo-car) — not faithful.
5. **Platforms/boost:** the collision converter strips info bits 0x10/0x20/11–15, so emit a side table
   Rush 2 polygon → original 2049 info. Record the contact polygon in R2 func_8006F704 at `before_vram 0x8006F99C`
   (poly = `ctx->r25`, car = `MEM_W(sp+0x148)`, output point `ctx->r17` with y = `MEM_F(r17+4)` < 1.0), clear it at
   `before_vram 0x800706DC` (`$a0` = car) and run the func_800E1F80 port at `before_vram 0x800706EC` (`$s0` = car,
   after drag, before func_8006A02C) with +0x218 vel, +0x2CC matrix, +0x5B0 mass, +0x638 dt, +0x224/+0x22C position,
   +0x5DC wheel heights, +0x594 polygon indices, +0x58 wheel forces, +0x11C force. Platform velocity:
   `World::platform_velocity(group)`.
6. **Triggers:** in the same per-car hook (once per frame to match the edge semantics; occupancy bits make per-tick
   equivalent), for each wheel look up `side_info[MEM_HU(car+0x594+2w)]` and call `World::car_on_group(car, info>>11,
   hit)` for every trigger group (skip wrecked cars: +0x648). Rush 2 cars 0..7 map to bits 24..31.
7. **In-place animations:** placement nodes of TROLLEY2/WINDMILL/WINDMILL2: `rotate_in_place(nodeMatrix, sub, dt)`
   per tick (2049: per frame).

---------------------------------------------------------------------------------------------------------------------

## 9. Uncertainties

- 0x801174B4 bit 8 = multiplayer, 0x80150DD0 = restart, 0x8014A110 values: inferred; races take the default path.
- Mass pairing 2049 +0x5C0 ↔ R2 +0x5B0 and collidable +0x7EA ↔ +0x804 are from offset deltas; meaning of 2049
  +0x6C4 and the +0x7CC == 2 state unknown.
- Sound "still playing" tests (func_800BF148) are left to the host; the port treats start sounds as finished.
- 2049 reads past the node array in position()/rotation() for a non-looping follower parked on its last node; no
  2049 race data reaches that state, the port clamps to the last node.
- The body-force strength under Rush 2's per-tick application depends on 2049's frame rate (§8.4).
- Mode 2 battle spawn rules (func_800ABCC8 refuses slot-4 objects without PTHD 0x10) are not ported.
