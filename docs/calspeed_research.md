# California Speed (N64) research

California Speed's N64 version (Atari Games, 1999) runs on the Rush 2 N64 engine, so its tracks are Rush 2 files
and race in Rush 2's HAWAII slot like the SF Rush and Rush 2049 tracks. Code: `src/calspeed/` (ROM, Games tab,
conversion, track select art), the hosting in `src/rush2049/track2049.cpp` and the track select in
`src/rush2049/track2049_menu.cpp`. Tools: `tools/calspeed/cs.py` (ROM reader, asset list) and
`tools/calspeed/disasm.py` (disassembles its main code). [V] = checked against the ROM or in game, [I] = inferred.

## 1. ROM

- **Dump:** "California Speed (USA)", 16 MB, game code `NCL`. The No-Intro `.n64` is byte-swapped; the app stores a
  big-endian copy as `calspeed.z64` (SHA-1 checked, `src/calspeed/calspeed_rom.cpp`). [V]
- **Code:** boot segment raw at ROM 0x1000 / 0x80000400; main code and data raw (not compressed) at ROM 0x1CD60 /
  0x80053710 up to the first asset at ROM 0xB1C70 (functions end at 0x800D20B0). F3DEX2 display lists. [V]
- **Assets:** 128 u32 ROM offsets at 0x8008DFBC (ROM 0x5760C), every one compressed with Rush 2's LZ variant
  (`track2049::rush2_lz_decompress`, `roms.lz(rom, off, False)`). [V]

| Assets | Contents |
|---|---|
| 0-23 | menus: 3 = track name banners (144 x 23 each), 6 = track select landmarks, 8 = Rush 2's leftover dioramas |
| 26-38 | shared hit objects (cones, lamps, trees, windows, buildings, helicopter, stealth bomber), animated frames `I1..In` |
| 39-61 | cars `<NAME>FRAME1` (one 2-LOD body each) |
| 62-77 | track t geometry (Rush 2 model container) |
| 78-93 | track t placement (Rush 2 format) |
| 94-109 | track t collision (Rush 2 format, its own surface types: section 4) |
| 110-125 | track t AI / checkpoint path (Rush 2 format plus a second route line: section 5) |

Tracks (t, prefix, name from its table at 0x8008A588): 0 MONTEREY Monterey, 1 SILICON Silicon Valley, 2 HWY1
Highway 1, 3 SF San Francisco, 4 CENTRAL Central Valley, 5 SC Santa Cruz, 6 SD San Diego, 7 CLOVER Clover, 8 LA Los
Angeles, 9 SHASTA Mt. Shasta, 10 YOSEMITE Yosemite, 11 MOJAVE Mojave Desert, 12 FUJI Fuji, 13 OVAL Oval, 14 CALI
California ("Cal" in the table), 15 SA San Andreas. Tracks 7, 12, 13, 14 and 15 are loops; the rest run from a start to a finish
(`rush2::calspeed::point_to_point`). [V from the paths: checkpoint flags]

## 2. Per-track tables

All in the main data, Rush 2's layouts (`src/calspeed/calspeed_convert.cpp`):

| Address | Contents |
|---|---|
| 0x80094C2C | f32 per track: cloud scroll |
| 0x80094C6C | f32 per track: sky parallax |
| 0x80094CB4 | RGB per track: fog colour |
| 0x800C4C80 | u8 per track: visibility region count |
| 0x800C0680 | 16-byte region masks, the tracks' tables back to back |

## 3. Conversion for the host slot

- **Geometry:** the track's container merged with the shared object containers its placement uses, plus the empty
  models and textures Rush 2 looks up by name (`<prefix>FINISH`, `<prefix>FINISHB`, `CHKPNT`, `FINISH`), through
  `track2049::merge_rush2_containers`. A Rush 2 container's image data ends where its texture-load lists start, so a
  G_DL to the first load list points at the image chunk's end; the merge maps an address to the chunk that starts
  there (`merge_models` `new_addr`), or every model calling that list draws texels as commands (RT64 then stops on an
  unknown GBI). Behaviour ids are cleared (California Speed's hit-object kinds mean other things in Rush 2). [V]
- **Placement:** the tree takes the host's name. Hit objects (`TREEHIT*`, `PLUMHIT1I1`, `CON17HIT1I1`,
  `LAMPHIT*`, `LIGHTHIT1I1`) become Rush 2 breakable records drawn with their first frame through SF Rush's record
  redirects (`track1::set_redirects`); windows and buildings stay static; the helicopter and stealth bomber draw
  nothing (`CSNONE`). [V]
- **Cache:** `track_cache` entry 2000 + t plus the blob `calspeed_track<t>_<prefix>`, keyed on the ROM and the build. [V]
- **Track select:** ids 64-79 (`rush2::calspeed::first_menu_id`), after the custom tracks; the screen's tables are
  at 0x80310000 (names +0, scales +0x200, cloud heights +0x400, logos +0x600, carousel +0x800 (128 x 0x1C), circuit
  instances +0x1600 (80), custom track strings +0x1800, California Speed strings +0x1C00), us.toml repointing the
  instructions. BACKWARD isn't listed on its tracks and LAPS isn't on the point-to-point ones. [V]

- **Cars under the course:** Rush 2's tracks lie over an out-of-bounds floor (surface 7: a wheel on it wrecks the
  car, which puts it back on the road); California Speed's have none and their ground runs on below the roads
  (Monterey's to y -252). A drone glancing off Monterey's ramp at (0, -10, 5630) flew over the left wall and drove on
  200 ft under the course. `rush2_calspeed_car_frame` (calspeed_rom.cpp, hooked with the car frame callback at
  0x8005CFB0, `car_update_draw_nodes_8005CFB0`) wrecks a car (`car_wreck_trigger_8006E1A8`) that has stayed more than
  60 ft below the nearest racing-line point (the path's spine, its branches (the alternate routes, which on Monterey run up to 127 ft below the main road) and AI lanes, height weighted 0.5) for 2 s of race
  time, as surface 7 would. A road under another one is nearer its own points, so a car on it isn't counted. [V:
  Monterey, every drone finished]

- **Breakables:** Silicon Valley's 71 cones and 41+ trees need more than the 40 breakable groups Rush 2's table
  holds, and loading it crashed (docs/rush2049_research/placement.md, Breakable groups); `src/game_fixes.cpp` gives
  the table 0x82 groups. [V: sim]
- **Fall floor:** Rush 2 puts a car back below y -190 (`race_stuck_wrongway_update_8008CDA4`); Silicon Valley's roads
  go down to -306, and its cars were put back every 7.5 s onto road still under the floor. On California Speed's tracks
  the floor is 300 ft below the racing line's lowest point (`rush2_calspeed_fall_floor`). [V: sim]

### Extended scenery

California Speed built its tracks only as far as its camera looked. A point-to-point track's road stops a few feet
behind the start line (the game doesn't let a car turn round), so turning round in Rush 2 shows the void, and its seas
are patches whose edges show as holes on the horizon, more so with the Draw Distance option. The Games tab's
**CS Extended Scenery** option (on by default, `calspeed_scenery` in games.json) fills these in when a
track is converted (`src/calspeed/calspeed_scenery.cpp`, `include/calspeed_scenery.h`): `add_scenery` changes the
track's own container (asset 62 + t), the converted placement and the remapped collision before the merge. The
converted track is cached apart from the plain one (track_cache key 2100 + t, blob `..._scenery`), and
`src/rush2049/track2049.cpp` converts again when the setting changed since the track was loaded, so a change takes
effect from the next race.

- **Reading the track.** The track's section records (those drawing a model of its own container) are run through
  their lists into world triangles (world = record position + vertex / 16 x the record's rows). California Speed's
  lists set their state with only a `G_DL` into a texture-load list (the container's [7]..[8] range),
  `G_SETOTHERMODE_L/_H` and `G_SETCOMBINE` (counts over Monterey's lists: DE 497, E2 188, E3 160, FC 128, E7 73, no
  inline loads), so each triangle's state is those words; new lists replay them and draw with the track's own
  textures. The new models are appended to the container (vertices and lists after its end, then a new model table
  with the texture and palette tables copied after it and a new sorted name table); the merge copies the whole
  container, so their pointers move with it. A load list's render tile (F5/F2 for tile 0) gives the texture's size and
  wrap, and its texels (the SETTIMG before the LOADBLOCK, the palette before the LOADTLUT) its mean color.
- **Behind the start** (point-to-point tracks). The cut is just inside the road's back end: along lines parallel to
  spine point 0's direction (every 5 ft within 30 ft of the spine), the furthest back the floor triangles reach; the
  most forward of those near the spine's own, so the whole road is there (Los Angeles' end is slanted by 57 ft). The
  triangles crossing that plane within 260 ft of the spine give the cross-section: each piece with its ends' texture
  coordinates and colors and the rate its texture coordinates change per foot going back (the projection of the back
  direction on its triangle; zero on a clamped axis). Pieces facing along the road (|normal . forward| > 0.6: tree
  cards, building ends) are left out, as they would sweep into walls, and so is the water on a track with the sea fill.
  The sweep goes straight on (texture coordinates continue the triangles' own mapping, so it lies over whatever
  California Speed built behind the road without showing) until 20 ft past the end of what was built there (coverage
  in 10 ft bins up to the first 40 ft gap), at least 40 ft past the barricades, then bends on a 600 ft radius (eased in
  and out over 120 ft) and rises 10 ft over a crest 250 ft on and drops 110 ft by 900 ft on. The bend is the best of
  six (70 degrees, 91 and 45, either way): the fewest track triangles inside its corridor past the straight part, then
  the fewest clear sight lines from the grid (12 eyes: 20 and 60 ft ahead of spine point 0, 12 ft either side and on
  it, 5 and 14 ft up) to its end's cross-section, through the track's triangles and the sweep's own. When every bend
  leaves its end in sight (San Francisco starts on the Bay Bridge's approach over open water) the road goes on 9,000 ft
  with a 15 degree bend instead, past the farthest the game draws (8,000 ft at Draw Distance 4x). Each 1,000 ft of path
  is one model (`SCNXBACKnn`). Texture coordinates are taken back by whole repeats per quad to stay in s16.
  Two fixes for what the cut catches of structures above the road: a flat surface over 12 ft up that 20 ft ahead
  (same state and height) is much wider and reaches over the spine belongs to something crossing over the road (Los
  Angeles' wide underside 25 ft up, whose diagonal edge the cut clips a corner of) and is left out, as it would sweep
  into a strip hanging in the air (raised ground beside a road widens ahead too but never reaches over it: Monterey's
  stays). And on a structure's underside (over 10 ft up, facing down) running a little off the road's direction (Los
  Angeles' overpass decks, 16 degrees), the texture axis that changes much more across the piece than going back,
  while the other changes faster going back, stops changing going back (else the stripes slide sideways off the deck
  along the sweep), and a short piece of a run of one state (a squeezed seam strip, -318 per ft) takes the run's
  longest piece's rates. The road's own pieces keep their small drifts, which keep its lane lines meeting.
  Los Angeles only: its overpass decks cross the road 16 degrees off its direction and end square to their own
  heading, up to 76 ft behind the cut on one side. Swept straight back from the cut, the copy kinked away from the
  stock deck and lay in its plane over that wedge (it flickered through it). So a raised structure there (pieces over
  10 ft up joined end to end, with a floor among them) goes on along its own heading (from its upright faces'
  facing, about 0.29 ft across a ft) past where the stock one stops, then turns onto the road's over 300 ft
  (`drift_shift`), and each end of each piece starts where the stock surface under it stops (planes behind the cut
  every 2 ft, while one has a piece of the same state through that end's point). Those decks have no collision
  (overhead, out of reach). Its left edge (- side: the barrier 21 ft out and the hedge behind it) has the same trouble
  at road level: the stock road goes on up to 56 ft behind the cut, narrowing (the barrier comes in to 14 ft out, the
  hedge to 17), so straight copies stood beside the stock ones as a second rail. Each piece of that edge is followed
  plane by plane behind the cut (same state, overlapping heights, within 3 ft across of the last), starts where its
  stock copy ends, and turns back out to its own line over 30 ft (`pull_d`, `pull_dl`), with type 5 walls along the
  turn.
- **Tree cards.** The upright cards beside the road within 240 ft ahead of the cut (vertical, facing along the road,
  outside the road's width: California Speed's tree and bush planes) are copied back along the sweep every 240 ft,
  each moved by the path's frame at its distance (position across, height over the road and turn kept), so the
  verge goes on behind the start, past what California Speed built there (`SCNXTREESnn`).
- **Tree walls.** A tree wall (a surface facing across the road, 12 ft tall or more, its texture green, outside the
  road) that starts up to 150 ft ahead of the cut, so the cut misses it, goes on too: the cross-sections 10, 25, 50,
  90 and 150 ft ahead give it (no piece of its state within 15 ft across at the cut or on a nearer plane), and it is
  drawn straight back to the cut and swept from there, its texture going on as it went (Yosemite's two at 75 and 99 ft,
  Santa Cruz's at 63). Only where the cut's ground reaches it (within 15 ft; Monterey's at 190 ft would stand past its
  grass, which ends at 54, and Santa Cruz's at 253 in the sea), except on Silicon Valley, where its far-left tree wall
  at 128 ft stands behind the 10 ft wall the ground ends in at 85 (hidden from the road); and only when its texture
  moves going back (Highway 1's green bank holds still and would smear). One with a track collision wall between it
  and the spine is out of reach and gets no collision. Santa Cruz's right side takes the same exception for walls
  stopping within 25 ft of the cut: its tree wall at 108 ft and the hillside at 227 ft stand behind its 30 ft tree
  wall at 63 and stopped 10 ft short of the cut. There the cut's ground ended at 61 ft, 2 ft short of that wall, with
  its foot 1 ft over the ground, and the trees behind showed through the hole along its whole length: the ground goes
  on out to the wall on its own slope and the wall's foot down to 1 ft under it. That ground's last authored edge slants
  across the cut (0.5 ft ahead at the wall, 0.5 behind at the rail), so its sweep starts from that edge's own vertices
  (`skip`). Mount Shasta's right tree wall, on its green hill's crest 74 ft out, ends 337 ft ahead of the cut and
  stopped abruptly in the view back: a plane 338 ft ahead (Shasta only) carries it on.
- **Sides that stop short of the cut** (Monterey's left, Mount Shasta's right). Monterey's grass ends 54 ft out at the
  cut but runs to its tree wall 190 ft out 40 ft ahead; Mount Shasta's green hill and the trees on it end 50 ft out at
  the cut. That side's outer profile comes from the nearest plane ahead (20 to 250 ft) where it runs on furthest:
  from the cut's edge outward the ground's chain of pieces (each starting where the last ended, the one reaching
  furthest out, or a jump to one starting lower) and the walls at its end. A piece joining within 2 ft starts
  exactly where the last ended (Shasta's hill had 1 and 2 ft gaps, a line of sky across it), and the walls' feet go
  down to 1 ft under the chain's ground where they stand (Monterey's tree wall stood 6 ft in from the grass's end with
  its foot half a foot over it). It is drawn straight from that plane back
  to the cut, moved up or down so it meets the cut's ground edge exactly (taken as it stood on its plane, Monterey's
  stood 0.9 ft over the cut's grass and the sky showed between the two strips), and swept with the rest. Only on those
  two sides: run on every track it changed others (Highway 1, San Diego, Los Angeles) that needed nothing.
- **Checking offline.** `tools/calspeed/scenery.py` renders with back faces culled and textures filtered as the game
  draws them (`glb_view.py --cull 1 --bilinear`): drawn both ways, a surface facing the wrong way showed in the render
  and was a hole in game.
- **Past the reach** (Central Valley's right, Mojave Desert). The cross-section takes 260 ft each side; the authored
  data at the cut past that is swept too where it matters: Central Valley's tree wall stands 279 ft out on the right,
  77 ft tall, all along the road (cut off at the cut, it stopped there), and Mojave's right slope runs on to 310 ft.
  No collision behind a track wall on that side (`past_reach`).
- **Mojave's canyon walls.** They stand off the road from 600 ft ahead, past the cross-section's 260 ft reach, so the
  cut never meets them. Each side's profile out to 1,500 ft from the road is taken at a plane where its wall is whole
  (+ side 1,500 ft ahead; - side 2,100 ft ahead), in across and height from the spine there (the road winds and
  climbs), from where the swept ground ends on that side (the authored data's own edge at the cut: 310 ft on the +
  side with the ground past the reach, 241 ft on the - side), lifted or lowered to that edge. Rules, each from a hole
  seen in game:
  - One unbroken profile: each piece starting where the last ended. The - side's plane climbs to 133 ft at 393 ft
    out, then a valley starts 124 ft under the road at 517 ft (sky showed through the break): it goes straight on to
    the nearest piece out that starts no more than 30 ft lower (the face at 648 ft), the valley left out.
  - Nothing past 3/4 of the bend's radius on the inside of the bend (the + side here): it folds over itself there (the
    + side's face at 743 ft twisted into a ribbon with the sky under it). That side ends at 450 ft, its slope 50 ft up.
  - The walls rise out of the flat ground: at the cut every piece lies at the edge's height, full shape 600 ft back
    (`rise_len`). A wall standing at the cut had an open end beside authored land that has nothing out there, and
    walls carried forward of the cut into the authored land stood as floating slabs.
  Only the - side keeps a canyon profile (from the plane 2,100 ft ahead). The right slope's outer edge (past 239 ft)
  is taken from 3 ft ahead of the cut, its ends on the authored vertices there.
  The distant canyon cutouts (a wall strip of three rows of quads: the + side's from 602 ft ahead, 1,029 ft out; the
  - side's from 1,584 ft ahead, 1,552 ft out, its rocky top a second texture over the first) end in a column of
  vertices facing the start, and the sky showed past it. Each goes on from that column itself, straight back along the
  road to 4,000 ft behind spine 0 (`SCNXCUTnn`, one model a 1,000 ft, no collision): each band takes its texture
  state, coordinates and rate along the strip from the authored end triangle holding both its column vertices (the
  strip wraps its texture per triangle), so the two share the column's edges.
  Each faces the road by rule: the game culls the side a triangle's cross product points to (Rush 2's world turns x
  the other way), so that product points away from the road. Taken from the strip's own triangles, the - side's
  summed the wrong way and was never drawn.
  Land runs from the drawn ground out to each cutout's bottom row (`SCNXLANDnn`, no collision). Stock Mojave's ground
  ends 440 to 690 ft out, well short of the cutouts; from the start's straight their feet sit under the ground's
  skyline, but from the road behind the cut (down to 100 ft lower) the sky showed under them. Every 25 ft along a
  cutout, a plane across the road through everything drawn (stock and swept, not the cutouts) gives the ground's outer
  edge: from the piece nearest the road, out through pieces starting within 2 ft and 3 ft up or down of the edge so
  far. The land joins that edge, tucked 3 ft in and 1 ft under it so the two never part, to the bottom row exactly.
  One texture a side, the ground state found most often at the edge (each plane's own put road-shoulder strips and
  striped slopes in it), at its median rate across the ground (short pieces across a texture wrap read many times
  it), running up the land's slope from the foot. Where the sweep's ground reaches past a cutout's line (the + side
  past 1,000 ft back, the road bending toward it, behind the crest) there is no land.
  Past the sweep's end they go on straight for 4,000 ft (`SCNXRUNONnn`, one model a 1,000 ft for the s16 vertices'
  2,047 ft reach): the crest hides the road's end, but not the end of a 300 ft wall. They stand behind the corridor's
  own walls and have no collision.
- **Barricades.** A row of Type III barricades (two posts on feet, three boards with orange and white stripes, amber
  lamps; a ROAD CLOSED sign on the face of the upper two boards) stands across the road `barricade` ft (45) behind
  where the road ends on the spine's line, plus the cut's lead on a slanted end. The road is the flat collision type
  0 pieces within 8 ft of the start's height and 80 ft of the spine, gaps of up to 25 ft taken in (Santa Cruz's spine
  runs on the dirt median between carriageways 5 ft apart in height). Each carriageway and each median between them
  (gaps over 1 ft) gets its own row of barricades up to 10.5 ft wide, so none straddles a level change; the sign goes
  on the middle one of each carriageway at least 0.6 of the widest run. The boards stay level over the higher foot and
  each leg reaches down to the ground under it. Their textures are made in the converter (stripes 32 x 16 RGBA16, the
  sign 128 x 32 IA8 with 5 x 7 letters at twice size: in I8 the intensity is the alpha too, and the dark letters went
  see-through) and loaded inline. The sign's text runs along -across (checked in game). They are shaded like the road
  under them (`SCNXBARRICADE`). Highway 1's stand on the stock road 420 ft ahead of spine point 0 instead, on the grid's
  side of the hill's crest (behind it, from the grid 1,250 ft ahead and 40 ft lower, they were out of sight): over the
  road's own profile there, the carriageway shifted with the spine across, and the invisible wall across the road
  moves with them (a car backed from the grid stopped 7 ft short of them).
- **Collision behind the start.** Everything drawn has collision, along the whole sweep. A piece takes the surface
  (flags and info) of the collision polygon under it at the cut; one with none there takes its nearest flat
  neighbor's if flat (else type 0) or is a type 5 wall (facing as its triangles did, turned with the path). Collision
  needs no texture, so pieces on one line of the profile with the same surface are one polygon, a piece lying on one
  already taken (a decal) is dropped, and end-to-end pieces join while their shared point stays within 0.3 ft of the
  joined line (rounded curbs and banks); slivers under 0.1 ft go. The track's own walls go on too, as its collision
  has them: California Speed walls its roads with invisible walls far taller than what is drawn (Monterey's stone wall
  is 3 ft, its collision wall 88 ft: polygons 1325/1531), and a car's body points pass over a wall only as tall as the
  drawing (it sank into the stone wall). Every collision polygon of a type over 2 crossing the plane 3 ft ahead of the
  cut (they stop about at it) gives a piece with its flags and facing (a quad's two triangles join into one); a drawn
  wall lying on one of those is left to it. The path is cut into segments shared by every piece
  (so neighbors share edges): a segment goes on while the sweep's middle and both edges stay within 0.25 ft of
  straight lines (coarser only if the polygons would pass the 13-bit polygon numbers of a leaf entry). A segment quad
  twisted by more than 0.15 ft (the bend and the grade twist wide sloped pieces) goes in as two triangles, since a
  file polygon is flat. Pieces with no surface under them at the cut also get a 20 ft lead ahead of it (the track's
  own collision can stop short of the cut off the road). An open outer edge (the profile's last piece is ground) gets
  a wall from 10 ft under to 30 ft over it; type 5 walls stand across the whole sweep at the barricades (30 ft under
  the road to 60 ft over it) and at its end. The converter checks that every quad drawn in the sweep lies on one of
  the polygons (report line "drawn quads without collision"). They go after the file's own polygons (POLY, VERT and vertex
  lists grow; the vertex list offsets stay under 64 KB). Every quadtree quadrant they reach gets a leaf of its own polygons
  plus the new ones, or, past 36 polygons (the queries' u16[36] buffers), a new node splitting it (boxes split toward
  zero as California Speed's builder did; a new polygon filed by its x/z outline, an old one by its box, with 2 units
  to spare, so either rounding finds them; a node made to split takes as its box the cells that reach it, see below). Those leaves can't go in the leaf section: Silicon Valley's is 65,478 bytes of its 16-bit reach. They
  go after the vertex lists behind a table of u32 offsets from the leaf section (4-byte aligned from the file's start);
  a node mask bit 4 + q says quadrant q's child is an index into that table. The root's byte +2 gets bit 6
  (`scenery_collision_tag`; Rush 2049's converted files use bit 7), and the four leaf fetch hooks of
  `src/rush2049/track2049_collision.cpp` read the table on such a file. Rush 2's own walk is kept otherwise. A leaf's
  count byte is its number of polygons, not of u16 entries (a run entry covers up to 8): the readers stop once they
  have that many, so a count of entries leaves the last polygons unread (the car fell through the new road). California
  Speed's own wall collision port (`src/calspeed/calspeed_walls.cpp`, out of the build for now) reads leaves itself,
  so it goes through the same rule (`rush2::collision::leaf_offset`).
  The root's box also has to hold the new polygons: `collision_node_find_cached_8006CE28` climbs from a cached node
  until the cell is inside a node's box and gives a cell outside the root's box no node (no floor, no walls), and
  most tracks' roots end just behind the start (Monterey's at z -8103, the barricades stand at -8140). The root's box
  grows by as much on both sides of each axis that needs it, so min + max and its split lines stay, and its edge
  quadrants take the new cells; polygons are filed by the cells that reach each quadrant (Rush 2's descent splits on
  a node's box but never checks it, so a node's edge quadrants take its region past its box). A node made to split a
  full leaf takes those cells as its box, so its splits divide all of them (with the quadrant's own box, the cells
  past it would all land in the new node's edge quadrants again) and the cached walk's climb still stops at it only
  for cells that reach it.
- **The sea** (Highway 1, San Francisco, Santa Cruz, San Diego). Water is the triangles whose texture is mostly blue
  (mean blue over 0.3, 1.5 x red, 1.15 x green) and that face up; its level is the area-weighted median height. The sea
  is a plane 1.5 ft under it (under the track's own water, which covers it), mapped with the biggest water triangle's
  texture coordinates as a function of x and z, out to 9,000 ft past the track. It is made of 256 ft cells, left out
  where the track has anything other than water lower than 2 ft over the water level (tunnels, the shore), merged
  into rectangles: within 4,000 ft of the track in 3,840 ft tiles (`SCNXSEAnnn`, s16 vertices at 1/16 ft reach 2,047
  ft from their record), beyond in 15,360 ft tiles drawn at 4 times scale (the record's matrix). At the original
  Draw Distance the game skips a node more than 2,048 ft from the camera on an axis, so the far tiles show with the
  longer draw distances only, as California Speed's own big sections do. Levels: Highway 1 -178, San Francisco -173,
  Santa Cruz -740, San Diego -211. Collision: each rectangle of the near tiles is a type 3 polygon at the sea's
  level (Rush 2's water hazard: a wheel or body point touching it wrecks the car, which is reset). California Speed's
  own water mostly has none (Highway 1: 381,000 of 13.9 million sq ft has a floor under it; San Francisco 334 sq ft),
  so a car falling in sinks through it until Rush 2's reset.
- **Records.** One top-level placement record per new model, appended to the end of the top-level chain with an
  empty culling box: the visibility regions are the first records of the chain (as many as the track's region
  count), so they are never a region and never hidden.

Offline: `python tools/calspeed/scenery.py [TRACK...] [--views back,top,far,sea,sign] [--stock]` runs it outside the game
(tools/calspeed/scenery_test.cpp: it walks the new collision as Rush 2 does, from the root box check down, and checks
every new polygon is in the leaf at points across it (pulled 3% in from the edges) and that half a foot off each
point along the floor's normal a floor of that leaf holds it; it prints the new polygons' range) and draws views
with `tools/glb_view.py` (its `--eye`/`--at` perspective camera) into tmp/scenery/; left and right are the game's (Los
Angeles' overpasses stand on the right looking back in both), but the ROAD CLOSED sign's own texture reads backwards
there while it reads right in game. `--stock` draws the unchanged track from the same eyes. Every point-to-point track: 0
track triangles in the bend's way and no sight line to the end except San Francisco's long road; every drawn quad on
a collision polygon; collision checks clean.

In game: `RUSH2_TEST_COLLISION=1` runs the collision self-test on a scenery file too (src/rush2049/track2049_collision.cpp:
every floor probed with Rush 2's ground query `collision_ground_query_8006CF00`, half a foot off along the normal);
`RUSH2_TEST_COLLISION_FROM=<first new polygon>` details the misses from there on. On all 11 point-to-point tracks no
probe on a new polygon finds nothing (the rest find a surface stacked over or under it: the ground query keeps the
lowest within reach). `python tools/calspeed/scenery_drive.py [TRACK...]` backs a car from the grid into the
barricades (and in arcs to the sides) and reports how far it got, its lowest point and wrecks.

### Billboard trees

Most of California Speed's roadside trees are one flat see-through card built into a track section's list, so from
the side they thin to a line. The Games tab's **CS Billboard Trees** option (on by default,
`calspeed_billboards` in games.json) turns those cards to face the camera about their upright axis when a track is
converted (`src/calspeed/calspeed_billboards.cpp`, `include/calspeed_billboards.h`), after the extended scenery. Cached
apart (track_cache key 2000 + t, + 100 with the scenery, + 200 with the billboards; blob suffix `_bb`), and
`track2049.cpp` converts again when the setting changed, so a change takes effect from the next race.

- **Which cards.** A lone quad of a section model (two triangles sharing no corner with anything else of the model,
  LOD 0's own list, not a called one) standing upright (|normal y| under 0.1) as a rectangle (bottom and top corners
  level within 15% of its height), at least 6 ft tall, at most 1.6 times as wide as tall (wider ones are hedges and
  tree walls), whose texture shows see-through texels (8% or more of the card) and is on the track's list of tree
  textures (`trees`: each texture is named T + its texels' offset in the track's container, asset 62 + t; most of
  California Speed's textures have no texture record). The list was picked by eye from a sheet of every texture on a
  card passing the shape tests: left out are signs, cattle, buoys, lamps, poles, the Bay Bridge's cables, a face,
  pictures of cars, flowerbeds and pictures of a row of trees. Not turned either: a card another card crosses (a
  crossed "X" tree) or touches within 1 ft (a row), or that a solid upright surface crosses or touches (a slope, a
  wall); a see-through one (a tree wall behind the tree) doesn't count. A section drawn by a record that turns or
  scales it is left alone (the turn is in world axes). Every California Speed section has one LOD.
- **Counts** (`python tools/calspeed/billboards.py`): Monterey 165, Silicon Valley 129, Highway 1 507, Central Valley
  271 (with bushes and crop plants), Santa Cruz 405, San Diego 169 (with palms), Mt. Shasta 500, Yosemite 287, Mojave
  Desert 754 (Joshua trees, cacti, shrubs). San Francisco, Clover, Los Angeles, Fuji, Oval, California and San
  Andreas have none.
- **Drawing.** Each section with cards gets a copy of its list with the cards' triangles taken out (G_NOOP, or the
  G_TRI2's other half as a G_TRI1), then, per draw state (replayed as the extended scenery does), each card:
  `G_MTX push mul T(base)` (a static Mtx after the container's end), `G_DL turn`, `G_VTX` of its 4 vertices in its own
  frame (x across, y up, around the base's middle, its visible side toward -z: the side away from its triangles'
  cross(v1 - v0, v2 - v0), since the game culls back faces and the cross product points to the culled side in Rush 2's
  world; built the other way the turned cards were invisible), `G_TRI2` with the triangles' own
  winding, `G_POPMTX`; the list then restores the TLUT mode and ends. The model record's LOD 0 list points at the copy.
  The cards stay part of their section's scene node: no new nodes (the node table holds 1,024), culled and hidden with
  the section.
- **The turn.** `turn` is a two-command list, `G_MTX nopush mul 0x0D000000` and `G_ENDDL`. Rush 2's relocator
  (func_8007786C) and the merge (`walk_reloc`) rebase the pointers of top-level lists only and never enter a G_DL
  target, so its segment address stays as it is (a G_MTX in the section's own list would get the container's base
  added). `rush2_interp_root_modelview` (src/interpolation.cpp, func_8007C624 after the root modelview) writes the
  view's yaw-only matrix, `view_billboard_basis_8007BF2C`'s output at 0x800E7C78 (rows: (-b, 0, a), (0, 1, 0),
  (-a, 0, -b) from the look-at matrix 0x800E7CC8's +0x8 and +0x28), as an Mtx into the side buffer and sets segment
  0x0D to it (`G_MOVEWORD G_MW_SEGMENT`), once per view: split screen views each get their own. The Mtx takes the
  basis by columns (Mtx row r = basis column r), as `gfx_build_node_matrix_cols_8007B290` builds the game's own flag
  0x8000 nodes (row 0 from basis +0x0, +0x10, +0x20). Taken by rows it is the opposite turn about y, which faces the
  camera only while the camera looks along a world axis: the first build did that, and the cards stood up to 90
  degrees off. The basis shows a model's -z side to the camera, so a card shows its front. Rush 2 itself only ever sets segment 0.

Offline: `python tools/calspeed/billboards.py [TRACK...] [--review]` runs it outside the game
(tools/calspeed/billboards_test.cpp, which also walks every new list and checks each card's command sequence and the
turn list) and draws tmp/billboards/chosen.png (every card turned), flat_<reason>.png (the ones left flat) and, with
`--review`, textures.png (one card per texture passing the shape tests, to pick the lists).

### Track select landmarks

California Speed's track select shows each track's landmark (asset 6, `<NAME>NICHIMEN`) as a relief in a niche,
its surface facing -x. `lift_model` copies the model with its texture loads and turns it a quarter about z
((x, y, z) -> (y, -x, z)) so it stands on y like Rush 2's dioramas; it is unlit, so only positions turn. Scale =
17 / radius: Rush 2's flat dioramas sit at radius x scale about 34, and a landmark as deep as it is wide reaches
the camera at that size. The name logo is the 144 x 23 banner (asset 3) box-filtered to 128 x 32 CI8. Cached as
the blob `calspeed_menu`. [V in game]

## 4. Surface types

California Speed numbers its collision surface types (POLY flags & 0xF) its own way. Over its 16 tracks (counts
of polygons):

| Type | Polygons | Shape | Rush 2's meaning |
|---|---|---|---|
| 0 | 34833 | ground | ground |
| 1 | 6612 | ground, material 1 | ground (off-road) |
| 2 | 1611 | ground, material 2 | ground |
| 3 | 5745 | flat ground beside the road, material 3 | wrecks a wheel touching it ("water") |
| 4 | 258 | vertical, every one | sinks the car |
| 5, 6 | 17274, 5390 | walls | walls |
| 7 | 1416 | vertical, every one | out-of-bounds floor: wrecks a wheel touching it |
| 8 | 213 | vertical, every one | runtime "airborne" marker only |
| 9 | 314 | vertical, every one | none |

California Speed's wall query at 0x800AECC4 takes types 5-9 together. In Rush 2 the type drives the physics (0
road, 1 dirt, 2 grass: off-road drag and torque), only 5 and 6 are walls to the body probe, so its cars drove
through the 4, 7, 8 and 9 walls at the road's edge (Monterey's right-hand verge near (4000, -40, -450) is type 8,
385 ft tall) and wrecked on the type 3 verges. The converter (`remap_surfaces`) makes the type 3 verges grass (type
2) and the 4, 7, 8 and 9 walls type 5, and keeps the wheel materials (bits 4-7) as they are: their effects are
California Speed's (below). [V data; the drones' wrecks there, in game]

Material counts on ground types, all 16 tracks, (type, material): (0,0) 27313, (0,1) 1676, (0,3) 2180, (0,4) 115,
(0,5) 2315, (0,6) 998, (0,8) 172, (0,9) 64, (1,1) 6612, (2,2) 1611, (3,3) 5745. Rush 2's own tracks: (0,0) 19530,
(0,1) 372, (0,2) 81, (1,1) 3028, (2,2) 15, (3,2) 124 (no 3-15 on the ground). [V data]

### Wheel effects

In both games the wheel material only drives effects (the physics read the type). Rush 2's material 2 is water, not
grass: 124 of its 220 material-2 polygons are type 3 (water), and Rush 2 has no grass particle. The first port mapped
California Speed's verges to material 2 and its materials 3-9 to 0, so its grass threw Rush 2's water splash and its
dirt threw road smoke. [V data, in game]

Rush 2 (car +0x60C + 2 x wheel, stored by `collision_poly_extract_8006F704` at 0x8006FA90):
- `car_material_effects_update_800663CC`: dust bits (+0x7F4 0xF0000) when wheel 0 or 2 is on 1 or 2 above +0x76C 41;
  on 1, 2 and 8 (airborne) it returns before the skid section (0x80066544): no skid sounds, and the skid bits 0x606
  (copied to car state +0xE4, one per wheel) keep their last value above speed 41 and are cleared below it. So no new
  skid starts there; a slide carried onto them keeps its skid bits.
- `particle_emitter_spawn_8008270C` (per emitter $s0: wheels 0-3, 4 and 5 the car's own): 0x800828EC material 1 ->
  emitter 3 (0x8010D26C, DUST), 0x8008292C material 2 -> emitter 4 (0x8010D28C, SPLASH), else SMOKE / SMOKE_FAST by speed;
  a wheel under speed/4 10 throws nothing (0x8008279C).
- Tire marks: `tire_mark_update_800823B0` (car $s6, wheel $s7, $a0 skidding) lays one only on material 0 with car
  +0x6C8 = -1 (0x80082404); `tire_mark_start_800821A4` makes its polygon (0x80082300, SKID04 0x80113F50, color
  0x800C5634: black, alpha 0xC0) and `tire_mark_extend_80081FD4` stretches it (0x80082184). Marks come off the free
  list 0x800C5668 onto the used list 0x800C566C. `polygon_set_80053D28` copies the color word into each vertex, and the
  polygons draw TEXEL0 x SHADE (0x8007D574), so the vertex color tints the mark texture.
- `effects_and_marks_update_80083228` fades the used marks every frame: it writes each mark's alpha into the same color
  word and sets the mark's polygon again with it (0x80083404), so whatever RGB the word holds recolors every mark.

California Speed (car +0x670 + 2 x wheel, stored by its wheel probe at 0x800AF510):
- 0x8007ED84 (Rush 2's 800663CC): dust bits (+0x848) when wheel 0 or 2 is on {5, 8}, {3, 9}, 1 or 2 above +0x7C6 41;
  skid sounds off on those and 10. 8 is ground here, not an airborne marker.
- 0x800BCF9C (Rush 2's 8008270C, from the per-car draw 0x800BC914, which skips a wheel on material 2): material 1
  -> DUST (0x5D, model 0x8008868A), anything else SMOKE / SMOKE_FAST by speed. Its DUST and SMOKE models and textures
  are Rush 2's (asset 10; the textures differ only by re-encoding, values off by 1).
- 0x800BD4E4 (Rush 2's 800823B0): a skidding wheel lays a mark on material 0 or on {5, 8} (class 1) or {3, 9} (class
  2); 0x800BD84C (Rush 2's 800821A4) starts it, 0x800BDF60 colors it by class: 0 black, 1 0x80764C (dirt),
  2 0x3A6332 (grass), alpha 0xC0, with its own SKID04.
  So {5, 8} are dirt and {3, 9} grass. As in Rush 2 its 0x8007ED84 makes no new skids with wheel 0 or 2 on those, so
  a colored mark comes from a wheel 1 or 3 on the verge while wheels 0 and 2 skid on the road, or a slide carried
  onto it.
- 0x800BEF14 (one per wheel, callbacks 0x800BF15C-0x800BF1C8 set by 0x800BEE4C): the water spray. Material 2 with the
  driven wheels turning forward (+0x438, Rush 2's +0x3D4) shows the wheel's node (SPLASHRIGHT_ for wheels 0 and 2,
  SPLASHLEFT_ for 1 and 3, made under the body by 0x800BA8C4 (through 0x800BA7CC) with flags 0x103 | 0x2000 and
  primitive color 0xFFFFFFC8, 0xA0A0A0C8 on tracks 14 and 15), scaled by min(speed / 40 + 0.5, 2.5) at the wheel point (state +0xC0, Rush 2's +0xA8)
  + (0, r - 0.835 x scale, 0.9 - r), r the car type's front / rear wheel model scale (0x8007D1DC / 0x8007D230, Rush 2's
  0x800C0A6C / 0x800C0B74), and sets both models' texture (0x800D1DE0: LOD handle + flag 0x8000) to SPLASH01-04 by game
  frame & 3; 0x800C9C10 shows a node and 0x800CBA38 sets its primitive color. So California Speed's material 2 is
  water as in Rush 2.
- Not ported: 0x8009F67C adds 2 to the rumble level for a front wheel on {5, 8, 1} (part of its own speed-based rumble,
  which Rush 2 does not have), and 0x8007D640 acts on both wheels on 10 (no polygon uses 10).

The port (src/surface_fx.cpp, include/surface_fx.h): the probe hook keeps California Speed's material per wheel and
stores the Rush 2 material it acts like (1, 3, 5, 8, 9 -> 1 for the dust bits and skid sounds; 2 -> 2; others 0). The
particle hook (0x8008277C) gives 1 DUST, 2 nothing and the rest smoke; the mark hooks lay and color marks by
California Speed's rule with its SKID04 (CSSKID04), writing each mark's color into the shared word around every set
(start, extend, and the fade at 0x80083404); the race-load hook (0x80087154) makes four spray nodes per car
and the per-car draw hook (0x80082D94) shows and places them. The converter adds SPLASHLEFT_, SPLASHRIGHT_ and the
textures SPLASH01-04 (as CSSPLASH01-04) and SKID04 (as CSSKID04) from asset 10 (`Rush2Pick`). Other converted games
keep Rush 2's effects until they get rules of their own. [V in game: dust, smoke, spray, tan dirt marks beside black
road marks; grass marks logged with 0x3A6332, same path]

### Stacked roads: the subway tunnel

San Francisco's subway tunnel is a second road 50 ft under the street, from about (-1330, 9100) to (-370, 6090),
reached by ramps cut through the street's deck (down at z 8350-8230, 7480-7440 and 7120-7090, up at 8944-8816,
8045-7924 and 6735-6586). No AI lane or spine point runs in it. Under the Rush 2 engine a car driving it is kicked and
launched; three things in its collision were found (in game, tools in tmp/cs_tunnel: run.py drops the player's car
into the tunnel in a silent sim-mode run and logs the wheels, analyze.py summarizes):
- The exit ramps' side faces (1796 and 4020 at the first, x -1266..-1252, y 78-131) are type 0 standing on edge. A
  wheel within a foot of one, or under the ramp within 5 ft of it, took it as its road and the springs shoved the car
  sideways (0 to 84 ft/s in half a second).
- A ramp down is a slab over the tunnel road. Where it closes down onto the road the slab, 1-5 ft over the wheels,
  beats the road a foot under them (the probe keeps the nearest polygon the wheel is from 5 ft under to 250 over;
  the probe point is the tire's contact, compression = rest height +0x5CC, about 0.4, - h, clamped to 3) and the car
  is slammed up: at the slab's foot a wheel read 4.6 ft inside it (2332 at (-1156, 7436); 2320 at (-1036, 7092)).
- The cuts are closed off by walls facing down: sloped faces between a ramp's edge and the deck's underside (2392 at
  the second ramp down, normal (-0.62, -0.72, 0.32), its plane through the tunnel road at (-1155, 97, 7442); 178 at
  the first; 807 in the other tunnel) and flat ceiling pieces 2-4 ft under the street's surface (210, 211, 1084, 1085
  at z 8728-8944). The body probe takes a wall when a body point projects inside it along its normal and lies from
  5 ft behind its plane to 3.75 ft in front, so a car on either road got the wall response: 37 ft/s upward in one
  step with the rear wheels off the road (at (-1155, 96, 7447)).

Rules fitted to these (vertical faces to walls, a road under the wheel preferred over a slab just above it, walls
facing down dropped from the body probe) made cars fall through roads elsewhere and were removed. California Speed's
wheel probe is Rush 2's: its polygon test `func_800AE884` (ground types: the same -5 to 250 window, nearest wins), its
wheel probe `func_800AF1EC` and its ground contact `func_800A58A4`, which is Rush 2's `car_ground_contact_800703EC`
with the road hold's per-wheel factor built in (+0x314: 0.32 on the road, 0.5 - compression + c past 0.5 ft of it,
0.5, 1.5 and 0.9 by how far a lifted wheel is out; 3 and -3 clamps, 40 and -40). Its quadtree is built to 2049's rules
(every floor and wall listed for its cells under them, 9 floors missed under Rush 2's; no wall gaps either way). What
differs is the body: California Speed never calls Rush 2's body probe (its copy `func_800AFAFC` has no caller in the
main code); its ground contact runs its own wall collision, next section, which the tunnel's ceilings and sloped faces
don't trigger. Ported on 2026-10-18 and taken out of the build the same day: with it (and with the missed-wheel rule
below) the user found collision on California Speed tracks broken in general, hold on or off; the port is parked in
tmp/cs_tunnel/removed/calspeed_walls.cpp. The build runs Rush 2's body probe on these tracks. [V: disassembly; in game]

### Walls: California Speed's body collision

Rush 2's body probe (`car_wall_collision_query_8006FC94`, four calls per car from the ground contact, one per body
point) takes a wall (types 5 and 6) when the body point projects inside it along its normal and lies from 5 ft behind
its plane to 3.75 ft in front, then pushes the car out along the normal and adds a force on the point. A ceiling 3 ft
over a body point is "inside" by that rule, which is the tunnel's kicks. California Speed's ground contact
`func_800A58A4` calls `func_800B1624` instead (at its start, before the wheel probes), the cars' wall collision:
- `func_800B1624` (car): the leaf under each of the four body points (+0x284 in its car struct, Rush 2's +0x260;
  `func_800AE668` from the point's cached node +0x3E4 + 4i, Rush 2's +0x380; cell = x - 0.99 truncated for negative x
  as Rush 2's), each leaf once; `func_800B1548` runs every polygon of a leaf whose type (flags & 0xF) is 4 or more
  through `func_800B0E80` with one hit record {POLY, body point, depth} (0x800DCCE8, zeroed); a hit left in the record
  at the end goes to `func_800B0A04`.
- `func_800B0E80` (car, POLY, record): the polygon already in the record is skipped. The four body points, in the
  order 0, 1, 3, 2 (0x800B1878: round the car), less the polygon's first vertex (VERT units: s16 x 32 + 5 fraction
  bits, x 1/32 at 0x800DCCD8) and turned into its frame (the POLY matrix, row 1 = the normal). For each edge of that
  quad whose ends' heights have opposite signs: the crossing with the plane at t = |y0 / (y1 - y0)|, the fraction of
  the edge behind the plane (t when y rises along the edge, else 1 - t, the edge's direction turned toward the part
  behind), clipped to the polygon's footprint in its x/z: the crossing must be inside every polygon edge (edge 0-1 is
  the frame's x axis, so its test is -x1 z_c <= 0; the others by cross product, the closing edge back to vertex 0 by
  z_last x_c - x_last z_c >= 0), and where the edge's line leaves an edge the fraction is cut there. The hit depth is
  |fraction x (y1 - y0)|; the deepest edge wins and the hit point is the edge's first end when |y0| / |y1| < 1, else the
  second. A depth under 0.0005 (0x800DCCE0) is no hit. A car in state 2 (+0x840) holds the hit in the record: a second
  polygon's deeper hit has the held one applied first and itself retested against the moved car, a shallower one is
  applied at once and the held one retested; any other car's hit is applied at once.
- `func_800B0A04` (car, record): the car moves out of the wall by normal x depth: position (+0x248, Rush 2's +0x224),
  its copy (+0x808, Rush 2's +0x7B0), the four body points and wheel contact points (+0x254, Rush 2's +0x230), and
  the body points are copied to their previous positions (+0x2B4, Rush 2's +0x290). The hit point's velocity r x w + v
  (body offset +0xE8, angular velocity +0x40, velocity +0x34, all the car's frame) goes to the world (the car matrix
  +0x324 transposed, Rush 2's +0x2CC) and into the wall's frame; the force on the point, in that frame, is -v.x and
  -v.z x 10000 / mass (+0x614, Rush 2's +0x5B0) x 1/dt (+0x698, Rush 2's +0x63C) x 0.006 (0.015 when the polygon's type
  is under 4 or over 9), and -v.y x 10000 / mass x 1/dt x 0.1 + 8 / dt x (1 - |forward . normal|) (the car matrix's
  third row, 1.0 at 0x800DCCD0); back to the world and the car's frame it is added to the point's force (+0xB8 + 12 x
  point, zeroed by the ground contact, Rush 2's too). Walls of types 4-9 also record their type per point (+0x862,
  nothing of Rush 2's).

The port (`src/calspeed/calspeed_walls.cpp`, `rush2_calspeed_wall_collision`, hooked at `car_wall_collision_query`'s
entry) runs the whole of it over Rush 2's car struct when the probe is called for body point 0 on a California Speed
track and skips Rush 2's probe for all four points; the leaf comes from Rush 2's `collision_node_find_cached_8006CE28`
with Rush 2's cell rule, so the body sees the wheels' leaves. The leaf offset is read with
`rush2::collision::leaf_offset` (include/collision_polys.h), the leaf fetch hooks' rule, so it takes a converted 2049
file's 17-bit offsets and the extended scenery's far leaves. Every car takes the state-2 path (Rush 2's struct has no
such state). Left out: the per-point wall type (+0x862), which nothing of Rush 2's reads; Rush 2's probe plays no wall
sound (its 0x6F is the type 3/4 sinking) and its directional rumble (src/rumble.cpp) gets each hit's normal. The
converter's surface remap (4, 7, 8, 9 to 5) is kept: the port takes types 4 and up as California Speed does, so it
would take them unremapped too. The leaf lists are unaligned (a count byte, then u16 entries), so the port reads
them bytewise. Test: tmp/cs_tunnel/run.py (silent sim run with RUSH2_CS_WALL_LOG=1: drops the player's car onto the
street over the tunnel's start, onto the street before the first ramp (it drifts to the west edge wall) and before the
second ramp down, logs wheels, velocity and position; analyze.py flags lifts, slams and velocity jumps; the game log's
`[cswall]` lines are the hits). In that test the car stands on and drives over the ceiling pieces (z 8900 to 8680) with
level compressions, grinds the west edge wall (polygons 170-176, 996) without passing it (before the fix of the leaf
lists' unaligned entries it fell through) and drives the tunnel road end to end. Left as it is: the wheel probe's hop
at the exit ramp's slab foot (wheel 0 at compression 3 for a step, a 2.4 ft lift, near (-1039, 102, 7094)): the wheel
probe is California Speed's own. [V: disassembly; in game]

## 5. Paths

- **Format:** Rush 2's path file (`tools/rush2049/paths.py`), one per track (no backward races). After the four AI
  lanes come two more lists of `{u16 count, 3 x u16 0}` + count x {s16 x, y, z}, unused by Rush 2: two more route
  lines (701 points on Monterey; 620 and 635 on San Francisco, one on each carriageway of its bridge, up on the deck
  at y 180, x 2426 and 2518 at z -2400). Perhaps California Speed's reset line. [V layout, I use]
- **Route line (Rush 2's spine):** its points are about 40 ft apart (median 39-44 ft; 73-82 ft on Monterey, Santa Cruz,
  Fuji and California), where Rush 2's are 20 ft (19.7-20.0 on every race track). California Speed's loader
  (`func_800A2A44`) keeps the route header at 0x80142700 (+4 the points 0x80142704, +8 u8 branches, +0xC the branch
  records 0x8014270C: 16 bytes, +6 the main-line point a branch joins, +0xA its count, +0xC its points); Rush 2 keeps
  the same at 0x80111940 (`path_branch_records` 0x8011194C). [V]
- **Resets:** California Speed's tracker `func_8009BF34` keeps each car's nearest route point (per-car state +0x10E,
  branch +0x10C (-1 the main line), the previous at +0x110 / +0x10D; the nearest in 3D of the 10 points from the last
  one on). A reset (`func_800A4020`, a1 = 0) puts the car back on that point, with no move along the route; a1 = 1 (a
  car outside the collision grid, `func_800AE668`) uses the last checkpoint's route crossing instead. Nothing else
  resets a car: California Speed has no wrecks, and its only other caller is the ground contact `func_800A58A4`, when
  no wheel is down and the four wheels' heights sum under -3000 (a car fallen 750 ft under the world).
  (`func_8009F2E4` would reset with a0 != 0, but its one caller `func_8009F194` is only called with a0 = 0, the
  grid, from `func_80093678`.) `func_800A56C0` places it at the point + the slot's offset turned
  along the route (`func_800A4C44`: x = (dz, -dx), z = toward the next point, a branch's last point toward its
  main-line join; 0x800C8E00 turns the rows by (-dx, dz)), at the route point's height. The slot table 0x800D8630
  (12 bytes each) holds the grid (slots 0-7: x 0 / 12, z 60 / 40 / 20 / 0) and the resets (slots 8-15, = 8 + the
  car: x -2.4, 2.4, -16.8, -12, -7.2, 7.2, 12, 16.8), and every slot gets the track's shift 0x800D86F0 (16 floats:
  9, 6, -6, -3.6, 10.8, 16.2, 7.2, -6, -15.6, 6.96, -3.6, 7.2, -10.8, 3.6, 0, 0) on x. Rush 2's respawn
  (`respawn_place_car_80090A40`, docs/rush2049_research/checkpoints.md section 8) instead moves a car on by its speed x
  (seconds wrecked + 1.5) in 20 ft points (590-800 ft; 2 to 4 times that on California Speed's points), players along
  the route line and computer cars along their lanes. Ported as California Speed has it, for every car
  (src/calspeed/calspeed_rom.cpp): `track_route` is `func_8009BF34` run for each race car every frame from
  `rush2_calspeed_car_frame` (its candidates, forks, carried list and reads past a branch's end alike; it starts on
  the main-line point nearest the car, where California Speed starts at checkpoint 0's crossing + 1 ahead of its own
  grid); `rush2_calspeed_respawn_advance` (0x80091368) moves the respawn point on by nothing; and
  `rush2_calspeed_respawn_place` (0x800918E8) puts the car on its tracked point + the slot offset for the car and
  track along (dz, -dx), at the point's height (California Speed's route points sit 0.1-0.4 ft over the road, median,
  and it adds no lift where Rush 2 adds 1.5 ft), faced along the route. Not ported: the a1 = 1 reset (outside the
  collision grid; Rush 2 has no such test) and `func_800A4C44`'s road-polygon matrix (`func_800AF57C`), which tilts the
  side offset with a banked road; the no-road matrix is used. A car stuck at one spot is put back at that spot. [V code;
  the shift's sign from the grid: only z toward the next point keeps all 8 slots on every track's road]
- **Route heights:** the route line isn't always on the road: San Francisco's points 324-433 (the bridge, 3,700 ft)
  lie at sea level (y 2-6), 180 ft under the deck they follow in x and z, in the middle of its two carriageways (the
  deck's collision is one polygon over both and the middle; the lanes climb the cables to 400 ft at the towers).
  California Speed's reset puts a car on the point's height, and its wheel query (`func_800AF1EC` -> `func_800AE884`:
  the wheel's height over each polygon's plane along its normal, the polygon's matrix at +4 in s16 / 16384, taken
  from 5 under to 250 over, the lowest wins) only finds road under a wheel, and the collision has nothing under the
  bridge, so a reset there would drop the car and reset it again when it is 750 ft down. The real game never gets
  there: the bridge's sides are type 6 walls 13-658 ft high (337 polygons) that keep a car on the deck, and it has no
  wrecks, so neither reset cause happens on the bridge. Rush 2's wreck respawns do, and the exact port dropped the car
  there. So where California Speed's wheel test finds no road under the reset spot (`rush2::calspeed::ground_under`,
  its `func_800AE884` over the converted collision's ground polygons, types 0-2 as remapped, -5 to 250 ft), the car
  goes on the AI lane point nearest where it wrecked, 1.5 ft up as Rush 2 puts a computer car, faced along that lane
  (`func_8006D348` with $a3 = 0); everywhere else the reset is California Speed's. Route points with no road within 15 ft under or 5 over them: San Francisco 111, LA 95, Santa Cruz 61, San
  Diego 43, California 34, San Andreas 14, Silicon Valley 4, Monterey 2, Central Valley 1, the rest 0 (jump gaps
  included). [V data and code]
- **Lane speeds** are true mph. California Speed's own drone code reads them so: `func_8009C774` (0x8009C7FC /
  0x8009C828, the current and next lane point's u8 at +6, lerped, into state +0x10), `func_8009CF00` (0x8009D0B0 /
  0x8009D130) and its reset `func_800A4020` (0x800A4210) multiply the byte by 1.46667 (an immediate, lui 0x3FBB / ori
  0xBBBC), mph to ft/s, as Rush 2's driver does (0x800CF8E0). The port first divided them by 1.2 as Rush 2049's
  displayed mph, and that made the drones easy to beat: they follow the lanes as closely as Rush 2's (sim race, 7
  drones, x/z distance to the nearest lane: median 0.8 ft on Monterey vs 0.7 ft on a Rush 2 track), but all four lanes
  over the 16 tracks are 145 / 170 / 181 / 188 mph at the 10th / 50th / 90th / 95th percentile (Rush 2's circuits:
  111 / 139 / 160 / 163), so after / 1.2 the drones held 121-157 mph on wide roads a player drives near top speed
  (Rush 2's average car: 168 mph); Rush 2's lanes sit at its cars' top speed on straights. [V disassembly and data]
- **Driven speeds** (`rush2_race_lane_speeds` 0x80093290 and `rush2_track49_race_time` 0x80093298, src/rush2049/
  track2049.cpp; `rush2::calspeed::capped_lane_speeds`, calspeed_convert.cpp): California Speed's lanes are set for its
  cars, which grip about 3 times as hard (tire curve grip 7.8 against 2.6-3.2) and which its road hold keeps down over
  crests, and Rush 2's driver only slows where the lane does. Driven at full speed (3 g cap only) the drones ran wide
  and flew off crests California Speed's lanes take at 190-200 mph (California's (1030, 70, 7450): airborne at 130
  mph, 50 ft over the lane, landing at 164 mph in a wreck). Each lane point is capped at what its curve allows at
  2.5 g (circle through the points 60 ft before and after, seen from above; Rush 2's drones pull up to 2.4-2.6 g at
  the 99th percentile in the sim) and its crest or dip at 4 g (change of grade over 60 ft either side), then braking at
  40 ft/s^2 back from every point. California Speed's lanes also drop speed in one step (213 -> 117 mph at Santa
  Cruz's (-18000, -670, 5900)), which Rush 2's driver can't brake for; the braking pass spreads those. The race
  timer stays as it was: func_800924E4 works the checkpoint times out from the lanes at / 1.2 (the timed speeds are
  written at 0x80093290), and the driven speeds are written after it (0x80093298). Both go through the Car Speeds
  lane map as Rush 2 paths (x 1.2 with Rush 2049 speeds). The record seed (`record_seed`) keeps / 1.2. Tuned in the
  sim with Drone Cheating off (tools/calspeed/sim_wrecks.py --no-catch-up, tools/calspeed/lane_hold.py): wrecks not
  from a rollover, before the finish, per minute of race: 0.33 over the 16 tracks against Rush 2's 0.24 (7 tracks);
  the / 1.2 lanes on the five worst tracks 3.08 (now 0.48 there); at 3 g corners twice the 2.5 g count. No drone
  wrecks at speed off its lane. Rollovers (1.20 / min against Rush 2's 0.16) are the road hold's. [V sim]
- **Driven speeds under CS Physics** (same place): Full CS drives the lanes uncapped, as California Speed does.
  Partial caps them for its player's car: `capped_lane_speeds` with corners at 3.25 g and crests and dips uncapped
  (the drones run California Speed's road hold). Its player's car runs Rush 2's tire curves (front grip 2.6) on
  California Speed's tire load (capped at 1100 lb a wheel, `tire_load`), 3.28 g on the Rush 2 average car (108.4
  slugs), against the drones' 7.6 g (grip 7.8, 140.4 slugs) and 178 mph top speed against 168 (California Speed's
  torque scales 1.9 / 1.95 / 2.05 and gears 3.05 / 1.68 / 1.17 / 0.88, the drivetrain model of
  docs/rush2049_research/cars.md#10-car-speeds-v). Measured offline with tools/calspeed/ai_pace.py (every lane: the
  drones at lane x 1.05 within their car's top speed, acceleration and grip, against the best lap the player's car
  could drive on the same lane), drone time over the player's best: Rush 2's circuits 1.022 (1.007-1.045 per track),
  California Speed's tracks under Full Rush 2 1.032, under Partial uncapped 0.877 (0.76-0.95) and capped 1.006
  (0.975-1.07). A flat slowdown needed x 0.815 and left the tracks 0.85-1.12; the crest cap at 4 g left San
  Francisco and San Andreas 1.11. [V model]
- **Computer cars' resets:** California Speed's reset calls `func_8009DAA8` (0x800A41C4) after placing a car: the
  drone (lane state 0x80151D80 + 0x128 x the car's +0x83A) goes on its own lane's point nearest the reset spot,
  searched between the last and the next checkpoint's crossings (+0x24; +2 in game mode 6), +0x26 = -1, +0x20 the
  clock, +0 = 20.0, +4 = 0, keeping its lane (+0x28). Under Rush 2's driver, which puts a drone back once it is 100
  ft from its lane point or stuck, a reset onto the route line looped: California's lanes 0-1 run 200-270 ft from the
  route and lane 3 at (1500, -20, 6850), so a drone reset there drove back across to its lane and was put back again
  (55 of the track's 99 wrecks in one race), and slot offsets left drones against walls on Central Valley and San
  Andreas. So `rush2_calspeed_respawn_place` puts a computer car on the lane point nearest California Speed's reset
  spot, of its own lane unless that is more than 50 ft away there (then the nearest lane's), 1.5 ft up and faced along
  the lane as Rush 2 puts one, with +0x28 that lane, +0x24 that point, +0x26 = -1 and +0x20 the clock (+0 / +4 Rush 2's
  driver works out every frame). Players keep California Speed's reset.
  [V disassembly and sim]
- **Start grid:** Rush 2 lines the grid up behind the spine point where checkpoint 0 crosses the spine, 60 ft deep
  (slot table 0x800CC2BC, `car_place_by_case_8006DE20` case 0). A point-to-point track's road ends a few feet behind
  its first spine point, and on six tracks checkpoint 0 crosses within 50 ft of it, so the back rows (the player's)
  started off the road and fell. `move_start` moves checkpoint 0 to the first spine stretch at least 75 ft along;
  every grid slot then has ground under it on every track (checked offline: each slot against the track's
  collision). The stretch must also leave every slot 5 ft from the walls (types 5 and 6 after the remap, within 15 ft
  of the road's height): the slots reach 8.5 ft to one side of the spine and 17 to the other, and on Highway 1, where
  the spine runs near the right wall, two cars stood 2 ft from it, tipped onto it before the start and fell through
  the course. California Speed's own grid isn't traced. [V]
- **Laps:** a point-to-point track is raced over one lap; every drone finishes Monterey's lap (about 200-225 s). [V]
- **Lane gaps:** Rush 2's drone driver puts a drone back on the road once it is more than 100 ft from its lane point
  along the lane or beside it: `drone_steer_along_path_80074284` keeps the car's offset from the point in the lane's
  direction (state +0) and across it (state +4, both x/z); `drone_driver_80074990` sets car +0x810 at 0x80075238 when
  either is past 100 (or the drone has been stuck: state +0x24 >= 11 for over 1 s), and
  `race_stuck_wrongway_update_8008CDA4` (0x8008CF00, 0x8008D0B8) places it by case 2 and sets its wreck state +0x342 to
  2. California Speed's lanes have no points in the air: Monterey's jump is one 120-128 ft step on every lane, so every
  drone that flew it was wrecked over the landing edge (-118..-75, -11, 5729..5755), at any height, with the road hold
  on or off. Central Valley, San Diego and San Andreas also have 100-131 ft steps. The converter
  (`split_lane_steps`, calspeed_convert.cpp) splits steps over 90 ft into even pieces of at most 45 ft; steps over
  1000 ft (Yosemite's lane 1 jumping back 5581 ft at its end) aren't road and stay. The lanes' checkpoint
  crossings are worked out at load (`func_80092BC4`). [V: Monterey's jump wrecks gone in the sim]

### The end of a point-to-point race

- **After the finish line** a car keeps driving: the player's under their control until every player has finished
  and the race is over (game state 4: `func_800AAE20` clears every car's release +0x71C at 0x800AAF18, and
  `physics_car_tick_80075880` no longer steps a car without it). San Francisco's lanes end about 190 ft past its finish
  line (checkpoint 4, (3434, -293, -3748), flag 1) and the subway tunnel ends in a wall (collision polygon 1723, type 5,
  z -4030..-4059) about 300 ft past it, so a car that crossed the line at race speed hit it, and Rush 2's crash tests
  (`car_crash_damage_stage_800715E8`: impact force over +0x644, speed over 400 ft/s, spin rate +0x44 over 10) wrecked
  it. California Speed has no wrecks; once the race is over (byte 0x8014FE0B, set in the code at 0x80080850) or the player's
  own byte (0x80151C02 + index x 0x2A8) is set, `func_80082894` takes the pedals: throttle +0x418 = 0, +0x414 = 1,
  brake +0x41C = +0x614 x 0.009 x 5 (x 7 while +0x438 is over 120), with the road hold x 4 (not ported; Rush 2
  keeps its own ending). On its point-to-point
  tracks a finished car (car state +0xEA) skips Rush 2's three crash tests, as in a battle arena
  (`rush2_calspeed_finished_car`, calspeed_rom.cpp), and bumps off the wall instead. [V in-game: with player 1's grid
  put behind the finish line, an unfinished car hitting the wall at about 105 ft/s scored 334337 against its threshold
  390000; with the threshold lowered to 100000 the unfinished car wrecked there and a finished one didn't]
- **The records screen** (game state 6, `postrace_records_update_800A952C`) races the same track behind it:
  `race_run_full_800A63A0` sets the attract byte 0x800FAE6C and runs `race_setup_800A5ADC` without loading the track
  again, and `race_car_finish_or_respawn_handle_8009E180` (kind 0) starts each car through case 0 of
  `car_place_by_case_8006DE20` (at once, no glide) at spine point 0x80110BDC: the race load's pick from the menu
  track's demo list (`demo_start_pick_8008E6B4`, 0x800C456C, HAWAII's: the host slot's), an arbitrary spot on a
  California Speed track. On San Francisco it was by the finish line, so the cars drove into the tunnel's end wall,
  turned, wrecked and glided back over and over. `rush2_calspeed_demo_start` (at the start of func_8009E180, kind 0)
  starts them at checkpoint 0's spine crossing instead (path header copy 0x8010BCE8 + 0x2E), where the race's grid
  stands, on every California Speed track. The attract byte also turned off everything California Speed's
  (`on_calspeed_track`: road hold, smooth collision, walls, resets), so the cars drove that race with Rush 2's rules on
  California Speed's roads; game state 6 now counts as a California Speed race, and the cars' route trackers (resets)
  start again there. The screen only comes up for a time that makes the table, and the lap count it checks is the
  4-player copy at 0x802407F0 (src/players4.cpp moves 0x800C2274). [V in-game: San Francisco, the records race starts
  at (-1148, 9, 10145) and drives the course with no wrecks]

## 6. Music

`src/rush1/track1_audio.cpp`, as SF Rush's. California Speed's audio setup (0x80056AD4) loads the music bank
(.ctl) at ROM 0xB82BC0, its samples (.tbl) at 0xB8A390, a song file at 0xD69FC0 (`S1`, nine songs, each
LZ-compressed MIDI as SF Rush's) and the sound effect bank at 0xD73AE0. The block 0xB82BC0-0xD73AE0 is reserved
past SF Rush's in the ROM image and filled from the ROM, the .ctl copy lives at 0x80E16000 (below 16 MB: the audio
microcode reads its ADPCM codebooks with 24-bit addresses), and the songs are Rush 2 songs 29-37. California Speed
has no per-track songs: its options' Audio Track picks one (strings at 0x800D4658), so a California Speed track
plays a random one of its songs by default (`src/music.cpp`). Its menu song isn't identified. [V layout]

### Engine sounds

California Speed's engine code is Rush 2's (src/car_engines.cpp plays it; Rush 2's side is in src/engine_preview.cpp
and docs/functions/rush2_audio.md), with its own tables, constants and samples. [V code; not yet heard in game]

| California Speed | Rush 2 | What it does |
|---|---|---|
| 0x80056594 | 0x8008C61C | Starts engine a0 on slot a1: 0x801427C0[slot] = engine + 9 x slot + 1 (Rush 2: + 10 x slot), command 0x1FFE + slot |
| 0x800563DC | 0x8008C49C | Stops every slot's engine sounds, clears 0x801427C0 and 0x80149D44 |
| 0x800565FC | 0x800626FC | Command 0x1FFE / 0x1FFF: starts the slot's layers, sets 0x80149D44[slot] = 1 |
| 0x80056724 | 0x80062CC4 | Every audio frame: each slot's layers' volume, pitch and reverb mix (0x80134384[slot]) |
| 0x8005629C | 0x80062598 | Layer pitch: min(rpm / base, 2), 1 for sound 0x23 |
| 0x8005632C | 0x80062640 | Table indices: rpm / 1000 (0-10), (load + 80) x 0.025 (0-12) |
| 0x800564B0 | 0x800624E4 | Layer volume (below) |
| 0x800ADE48 | 0x8008C6E8 | Race start: the slot's rpm and load, then 0x80056594 with 0x80141568[slot] |
| 0x800ADEFC | 0x80064F70 | The slot's rpm (0x80168F68) and load (0x80134458) each frame |
| 0x8007EC08 | 0x8009E66C | Player engine start: rpm scale 0.8 (0x800F7798; Rush 2 1.0), load scale 0.3 (0x8016E150; Rush 2 0.3) |
| 0x8007EC70 | 0x800650DC | rpm = \|car +0x846\| x 0.8, load = car +0x458 x 0.3 (both as u16 & 0x7FFF); +0x846 is Rush 2's +0x7F0 (rad/s x 9.549 x 0.9, written at 0x80090EDC), +0x458 Rush 2's +0x3F4 |

- **Engine per car:** s8 table 0x8007D284 (car select order, read at 0x8009EDE0 into 0x80141568[slot]): SPORTSTER 5,
  MUSCLE 0, 486SE 2, PREDATOR 0, MANO 1, FIVE OH 4, MT DEW 6, HOPPER 1, FAIRCHILD 7, INSECT 3, MERCADO 7, GOLF CART 4,
  SQUIRREL 7, FORKLIFT 0, CONV 0, SLED 2, BAJA 6, OL TRUCK 3, CAMPER 7, SEMI 0, DOZER 1. Engine 8 is unused. There is
  no engine choice, so the port's car select hides ENGINE for these cars.
- **Layers:** sound 0x80058220 [id x 2 + layer] (-1 none, | 0x200 on slot 1, the same sample), pitch base
  0x80058198 [(id - 1) x 2 + layer]. Engines 0-8 on slot 0: 26, 2D, 2C, 30, 2B + 23, 2D, 2E + 2F, 29 + 2A, 27 + 28;
  bases (layer 0 / 1) 4000/3000, 5000/3000, 6000/3000, 4000/3000, 4000/4000, 7000/5000, 5000/5000, 5000/5000,
  5000/5000. Slot 1 (player 2) differs: engine 4 is 2B + 2B (no 23), engine 5 is 24 + 25, and engines 5, 6 and 8
  have bases 4000/4000. Sound 0x23 gets volume 0 and pitch 1 every frame (0x80056888), so it is silent and not played.
- **Volume:** (int)((int)(A[layer][rpm index] x B[layer][load index] x 1635 + 3270 x 6) x engine volume / 40), with
  1635 at 0x800AE3DC and 6 at 0x800AE3E0 (no code writes them; Rush 2: x 2000 + 24000), A = 0x80058354 (2 x 11),
  B = 0x800582B8 (2 x 13), the engine volume option at 0x80150B31 (0-40).
- **Samples:** the sound effect bank, ROM .ctl 0xD73AE0-0xD78C40 and .tbl 0xD78C40 (loaded at 0x80056E88; one
  instrument, 95 sounds, 44100 Hz). The engine sounds 0x24-0x30 are looped VADPCM (order 2, 4 predictors), volume 127, sustain
  127, no detune. The output rate is 22050 Hz, as Rush 2's.

The port reads these from the ROM once (`cs::make`) and plays every California Speed car's engine on the host:
local players 1 and 2's at a player's own volume (Rush 2's slot engine start is skipped for them,
`rush2_engine49_start`, as for 2049 cars; player 2 uses the slot 1 rows), other cars by position as
src/car_engines.cpp plays Rush 2's. Without the bank the cars fall back to their stand-in Rush 2 ENGINE values.

## 7. Cars

California Speed's 21 cars are car types 36-56 (`src/calspeed/calspeed_cars.cpp`; the type layout in
`include/car_types.h`, the shared car code in `src/rush2049/car2049.cpp`). Its car code is Rush 2's: the car struct is
Rush 2's with the fields from +0x5B0 on moved 0x64 later (Rush 2's mass +0x5B0 is +0x614), and the car init
`func_8009A300` is Rush 2's `car_init_setup_8008DBA0`. [V]

- **Names** (0x8008A6D4, 20 bytes each): SPORTSTER, MUSCLE, 486SE, PREDATOR, MANO, FIVE OH, MT DEW, HOPPER,
  FAIRCHILD, INSECT, MERCADO, GOLF CART, SQUIRREL, FORKLIFT, CONV, SLED, BAJA, OL TRUCK, CAMPER, SEMI, DOZER. The car
  select logos are drawn in Rush 2's style from these (`set_calspeed_logos`, texture names RCSCARn). [V]
- **Models:** asset 41 + n is a Rush 2 container holding only `<prefix>FRAME1` (two LODs) and its textures; prefixes
  Z3, CAMERO, FERRARI, DIABLO, MANO, COPCAR, M2, PROWL, SUPER, BUG, CIVIC, GOLF, DRONE, FORK, CADDY, SLED, BAJA, OLTRUCK,
  WIN, TRUCK, DOZER. `track2049::convert_body_car` makes it a Rush 2 car as a 2049 car is: the body as CSnFRAME1,
  Rush 2's other 36 parts empty, the sheen layers from the body; its textures renamed `CSn_<name>` (several cars share
  WINDOW and UNDRSDE2). Served as asset 0x80 + n (`car2049::car_asset`), dented by panel damage like a 2049 body, its
  damaged panels drawn with scuffed copies along California Speed's ramps (`paint_ramp`). The driver and underside
  textures load their own TLUTs in their load lists, so only the car palette is the game's. Cached as the blobs
  `calspeed_car_<n>`, keyed on the ROM and the build. [V]
- **Paint** (`func_800BAE2C(palette, c1, c2)`, RGBA5551 colors; its blend `func_800BAD6C` is 2049's `blend5551`, from a
  at t = 0 to b at 255): every car uses CARPALETTE (asset 25: a blue ramp at 0-127 plus fixed entries). Entries 0-62 =
  blend(c1, black, trunc(255 - 3.984375 i)), 63 = c1, 64-127 = blend(c1, white, trunc(3.984375 (i - 64))); 188-221 =
  blend(c2, black, trunc(255 - 7.5 (i - 188))), 222 = c2, 223-255 = blend(c2, white, trunc(7.72 (i - 223))).
  `rush2_car49_paint` (end of `car_build_textures_8008582C`) paints a California Speed car so with MAIN as c1 and
  ACCENT as c2; the car select hides STRIPE and STRIPE COLOR on them (`wings_menu.cpp` `Rows::TwoColors`). [V]
- **Default colors:** a new player record's colors (record + car x 8 + 0x536 / 0x538, rims + 0x53A) come from
  `func_8005E5C0`: c1 the RGB word at 0x800DC0D0 + 4n, c2 at 0x800DC124 + 4n, rims row 0 of 0x8007D3BC. The drones'
  (`func_800A6F88`) are picked from a run-time table at 0x8014A908. Here they are the nearest of Rush 2's 32 colors
  (`calspeed::car_defaults`), in the default rows and new side slot blocks. [V]
- **Per-car tables** (21 entries each, Rush 2's layout and meaning): steer base 0x8007CF20 (10000 for all), yaw
  damping 0x8007CF74 (1.0), name pointers 0x8007CFC8, type -> descriptor 0x8007D01C (u8), preload 0x8007D034 (0.05),
  mass 0x8007D088 (x 0x8007D47C = 1.0; 111.8 / 139.75 / 167.7 / 174.69 by class), pitch/roll inertia 0x8007D0DC,
  crash threshold 0x8007D134 (390000 for all; no code reads it), weight 0x8007D188 (0.5; `func_8009A8E0` takes its
  delta as itself minus itself, so it never changes the mass), torque map 0x8007D2AC (2 = HIGH for all), drive flags
  0x8007D2C4 (3), handling 0x8007D304 (4 for all), steer and yaw adds 0x8007D31C / 0x8007D348 (Rush 2's 0x800C0DF0 /
  0x800C0E1C, the same values), rims 0x8007D3BC. The handling value indexes both adds (`func_8009A838`,
  `func_8009A88C`), as Rush 2's TIRES does: TIRES = 4 (S1 D1). Rush 2's SUSPENSION has no counterpart (NORMAL), and
  ENGINE (the engine sound) is the most similar Rush 2 car's, heard only if California Speed's own engines (6. Engine sounds) can't be read. 0x8007D3FC / 0x8007D43C are per-player carousel ids
  (player x 42 + i, + 21), not colors. [V]
- **Descriptors** (0x800D6418: 18 pointers to 0xE0-byte descriptors) are Rush 2's 0xEC-byte layout less a few fields
  (`build_descriptor`): 0x00-0x53 the same; Rush 2's +0x54/+0x58/+0x5C missing (California Speed reads
  only +0x50, 16 in both games), taken from the Pickup; 0x54/0x58 = Rush 2 +0x60/+0x64 (drag, rolling
  resistance); 0x5C-0x68 = +0x68-+0x74 (tire curve pointers: 0x800D50F0 front, 0x800D5108 rear, much grippier than
  Rush 2's); 0x6C and 0x70 two grip factors (`func_800B8CE8` scales a tire's two force components by them; 1.0 on most
  cars, the Dozer 1.5 / 3.0), with no counterpart in Rush 2, whose +0x78 (rear lateral grip) California Speed lacks:
  +0x70 is folded into the car's own copy of the curves' grip (`calspeed_tire_curves`) and +0x78 is 1.0 [I which
  factor is lateral]; 0x74-0xA0 = +0x7C-+0xA8 (wheel positions); 0xA4-0xAC = +0xAC-+0xB4; 0xB0-0xBC = +0xB8-+0xC4
  (torque scales, the same 1.9 / 1.95 / 2.05 / 1.3 on every car); 0xC0-0xD0 = +0xCC-+0xDC (neutral, gears 1-4:
  3.05 / 1.68 / 1.17 / 0.88 on every car; `func_8008E748` reads desc + 0xC0 + gear x 4); no reverse (the Pickup's);
  0xD4 = +0xE0 (top gear, s16); 0xD8/0xDC = +0xE4/+0xE8 (shift thresholds). The cars differ only in mass, inertia,
  wheel layout, grip factors and box (0x800D60E0, 16 bytes each). [V layout]
- **Options** (Games tab): CS Cars (offered in the car select, after the other cars) and CS
  Drones (All Tracks, CS Tracks, Off: `rush2_car49_drone`). They have no unlock shop items.
  Online, a California Speed car needs every machine to have the ROM (`online_net.cpp`). [V]
- **CS Road Hold** (Games tab, `calspeed_road_hold`, default On, takes effect at once): off, its tracks
  have Rush 2's own gravity only (see Road hold). Custom tracks get it too with `[track] road_hold = true`
  (`rush2::track2049::mod_road_hold`, docs/custom_tracks.md#road-hold), so the option stays enabled without the ROM. `tools/calspeed/sim_wrecks.py` races the tracks headless with it on
  and off and counts the computer cars' wrecks and rollovers.

### Road hold

California Speed keeps its cars on the road with a force Rush 2 lacks, and its cars' values (grip 7.8, springs
-6800 to -18360, steer force 10000, yaw damping 1.0) are tuned against it: copied into Rush 2's physics they flip at
small steering inputs, and its tracks' hills throw every car several times higher than California Speed does. Gravity
is the same in both (32.2 ft/s/s x the 0x800D7380 table, Rush 2's 0x800CB580). [V]

- **Ground contact** `func_800A58A4` (Rush 2 `car_ground_contact_800703EC`; car fields +0x64 past Rush 2's here:
  compression +0x620 = Rush 2 +0x5BC) clamps each wheel's compression to +-3 and its rate to +-40 as Rush 2 does, then
  sets a per-wheel road-hold factor +0x314: in the air (compression < 0) 0.9 below -0.75, 1.5 below -0.5, else 0.5; on
  the ground 0.32 (double at 0x800D8740), or past 0.5 compression 0.82 - compression, first raising wheel (i + 2) % 4's
  factor by 0.2 when it is below 0.2 (only wheels 0 and 1 keep that: the later wheels replace theirs). A wheel that
  found no road while fewer than 3 did is put 1000 below it (clamped to -3). It also normalizes +0x3D8, respawns
  (`func_800A4020`) with no wheel down and the summed +0x258 below -3000, and ends in `func_80080FCC` (Rush 2 calls
  `car_collide_with_cars_8006F3C0` there). [V code, I field roles]
- **Wheel forces** `func_8008F23C` (Rush 2 `func_8006AFD8` + `car_force_sum_8006A02C`) adds, per wheel, the vector
  (0, -(weight +0x610) x factor, 0) to the car's force sum (+0x4, the car's frame): as it is (along the car's own down
  axis), or rotated from the world by the car matrix +0x324 (`func_800C9008`, as gravity is) when any wheel is more
  than 1.0 out of the road. It is a push on the whole car, not at the wheels (no torque). The factor is x4 once the
  byte at 0x8014FE0B is set (when the race is over, `func_80080850`; it also takes over the throttle,
  `func_80082894`) or the player's byte at 0x80151C02 + index x 0x2A8. So a car on the ground carries 4 x 0.32 = 1.28
  times its weight extra, and one well off the ground 3.6 extra (4.6 g in all), which is why its jumps are low. [V]
- **The port** (`rush2_calspeed_road_hold`, hooked in `car_physics_forces_800706D0` after `car_force_sum_8006A02C`,
  for every car on a California Speed track, not in the attract demo (0x800FAE6C, which starts without the track
  select that clears the California Speed track and could land on HAWAII, the host slot); `hold` picks the version,
  `Hold::Heavy`, `heavy_push`). California Speed's world as a heavier one, with the car built to match so it drives
  as on any other track, and a hold at the road. California Speed is built this way: its road hold is 1.28 g more on
  the road (2.28 g in all) and up to 3.6 more with the wheels well up (4.6 g), and its cars' springs per unit of mass
  are 2.27 times Rush 2's (-10620 against -3600 at 1.3 times the mass), their dampers 1.56 times (sqrt 2.28 = 1.51),
  so they sit at Rush 2's height under it. With `gravity_scale` = 1 + 4 x 0.32 = 2.28:
  - gravity x the road hold's factors (`heavy_factor`: a constant 0.32 per wheel on the road, off it California
    Speed's 0.5, 1.5 from 0.5 to 0.75 ft out, 0.9 further), the car's gravity +0x200 (the world gravity +0x2C0, a
    force of the car's weight, turned into the car's frame by `car_force_sum_8006A02C`) added to the summed force at
    +0x4; California Speed's easing of the ground factor past 0.5 ft of compression (its landing softener) is not
    kept: under a fixed spring scale it dropped the car's gravity below what the springs were set for in a dip, on a
    loaded corner or at a ramp foot, lifting the car and costing the tires up to a tenth of their grip (Gravity's
    slope trouble);
  - springs and anti-roll x 2.28, dampers x sqrt(2.28) (`rush2_calspeed_wheel_damper` rebuilds the load,
    `car_wheel_force_loop_8006ABB0` 0x8006AC8C-0x8006AD0C, for 0 < compression <= 10): the car sits at Rush 2's
    height and the suspension's damping ratio is Rush 2's;
  - the tires' load / 2.28 (`rush2_calspeed_tire_load`, hooked at 0x8006AD4C as $a2 goes to `car_tire_model_8006A2FC`:
    min(0x34($sp) / 2.28, weight +0x5AC), the game's cap): grip is Rush 2's;
  - direction: while every wheel is within 1.0 ft of the road, the whole extra (weight x the factor sum) square to
    the road (the mean normal of the polygons under the touching wheels, or under all four when none touch, turned
    into the car's frame by the car matrix +0x2CC as `push_car`), so hills, banks and grades pull along the road as
    on any other track and the springs see 2.28 x their Rush 2 load on flat road; California Speed pushes the same
    amount along the car's own down axis, so on a bank or on the walls of San Francisco's subway tunnel loop the car is pressed on as hard
    as on flat road. Until 2026-10-18 the extra was the projection of the scaled gravity onto that normal, which is
    nothing on a vertical wall: in the subway tunnel loop (route points 360-440, polygons 7120-7288, surface type 0
    material 6, spline flag 0x1060) cars had no hold, fell off and flew across and through its walls. Once any wheel is more than 1.0 ft out (a jump, or over nothing: compression
    clamps at -3) the whole extra pulls straight down, California Speed's rule;
  - stick: no amount of gravity keeps a car on the road over a kink (California Speed's roads are large flat
    polygons, a hill is a run of them): the car's velocity has to turn at once while gravity bends it over time, and
    at 2.28 g a 5 degree kink at 180 ft/s still lifts the car 1.7 ft for 0.4 s. So while a wheel has lifted
    (compression below 0) and none is more than 1.0 ft out, the car's velocity away from the road (+0x34 along the
    normal above) is taken out that step: a force of mass +0x5B0 x that velocity / the step +0x634, at most
    `stick_cap` = 6 x the weight (California Speed's 1.5 band is 6 g over its ground pull). A ramp lip gives far more
    separation than the cap takes back within the foot, so jumps still fly, a little shorter; a wheel lifted in a
    corner has next to no velocity away from the road, so nothing is added there; with four wheels down it never
    runs.
  Mass is unchanged: a lighter car under more gravity keeps the same weight on the springs but the same tire, engine
  and brake forces move it that much harder; steady-state weight transfer, grip, engine and brakes are Rush 2's, the
  suspension runs 1.5 times faster with a 2.28th of the body roll, and a car has to lift against 2.28 g to roll over
  or leave a crest: California Speed's own trade, and what keeps its narrow cars down on its banks. Springs x 4.6
  (the air's gravity: twice as stiff as California Speed's) kicked cars over at ramp feet: Monterey's jump, a 17
  degree crease at 185 ft/s, threw 6 of 7 computer cars into backflips (stock 3 of 5, x 2.28 2 of 5).
  `RUSH2_ROAD_HOLD_LOG` prints the player's car's compressions, factor sum and stick (x weight) every step a wheel
  is off the road.
- **Net** (`Hold::Net`, `net_push`, tried 2026-10-17; kept in the file). California Speed's push less what its
  springs hold up: a wheel on the road adds nothing, one off it California Speed's whole factor x weight / 4 (ramped
  over the first 0.25 ft, `net_ramp`), so with four wheels down the car is exactly Rush 2's. Cars stopped flying far
  (over a crest needing 1.5 g they hovered 0.05 ft up), but they still rolled over on the road: Santa Cruz's
  computer cars wrecked 91 times (137 rollovers) against stock's 103 (225) and Gravity's 11 (31); San Francisco 11
  against 5. The 1.28 g on the road is what keeps California Speed's narrow cars down, not the pull once they have
  left it. Earlier `Hold::AirPull` had taken the 0.32 off the air factors as well (0.18 for the first half foot), so
  it lifted and was then yanked: once a wheel is off the road its spring carries nothing, so the whole 0.5 is the
  net.
- **Gravity** (`Hold::Gravity`, ran until 2026-10-17; kept in the file). Heavy's heavier world as first built, with
  the car built to match so it drives as on any other track. California Speed is built this way: its road hold is 1.28 g more on the road (2.28 g in all) and up to
  3.6 more with the wheels well up (4.6 g), and its cars' springs per unit of mass are 2.27 times Rush 2's (-10620
  against -3600 at 1.3 times the mass) and their dampers 1.56 times (sqrt 2.28 = 1.51), so they sit at Rush 2's height
  under it. With `gravity_scale` = 1 + 4 x 0.32 = 2.28:
  - gravity: the road hold's factors (`road_hold_push` / weight: 1.28 on the road, up to 3.6 in the air) x the car's
    gravity (+0x200, the world gravity +0x2C0 turned into the car's frame by `car_force_sum_8006A02C`; Rush 2 sets
    +0x2C0 once at car init) is added to the summed force at +0x4 (`gravity_push`), leaving +0x2C0 to the wings'
    gravity scaling;
  - springs and anti-roll x 2.28, dampers x sqrt(2.28) (`rush2_calspeed_wheel_damper` rebuilds the load,
    `car_wheel_force_loop_8006ABB0` 0x8006AC8C-0x8006AD0C, for 0 < compression <= 10): the car sits at Rush 2's
    height and the suspension's damping ratio is Rush 2's;
  - the tires' load / 2.28 (`rush2_calspeed_tire_load`, hooked at 0x8006AD4C as $a2 goes to `car_tire_model_8006A2FC`:
    min(0x34($sp) / 2.28, weight +0x5AC), the game's cap): grip is Rush 2's;
  - hills: on the road the extra gravity's part along the road (the touching wheels' mean polygon normal, as
    `push_car`) is taken out, a quarter per touching wheel, so it fades as the car leaves the road and the whole
    extra pulls it down in the air. `extra_downhill` / `extra_uphill` keep a fraction of it where it pulls along /
    against the car's travel (+0x34, the car's velocity in its frame): 0 = Rush 2's hills, 1 = the whole heavier
    world; both 0. Its part across the road (banking) is always taken out.
  Mass is unchanged: a lighter car under more gravity keeps the same weight on the springs but the same tire, engine
  and brake forces move it that much harder. Springs x 4.6 (the air's gravity: twice as stiff as California Speed's)
  kicked cars over at ramp feet: Monterey's jump, a 17 degree crease at 185 ft/s, threw 6 of 7 computer cars into
  backflips (stock 3 of 5, x 2.28 2 of 5). Set aside for Net: the ground factor isn't constant (0.82 - compression
  past 0.5 ft, California Speed's landing softener), while the spring scale is, so in a dip, on a loaded corner or
  at a ramp foot the car's gravity fell below what its stiffened springs were set for: it was lifted and the tires
  (load / 2.28) lost up to a tenth of their grip; the along-road removal was weighted by the touching wheels, so a
  wheel lifting on a bank stepped a quarter of the extra's sideways part back in; and the suspension ran
  sqrt(2.28) = 1.5 times faster with a 2.28th of the roll everywhere, the price of the heavier world.
- **Damper** (`rush2_calspeed_wheel_damper`, hooked at 0x8006AD10, the same toggle; under Net only the jump limit
  below runs, the spring and damper scaling is Gravity's). The load is (anti-roll +
  compression x spring - v x damper) x a time-step scale, v being the wheel's velocity along the normal of the polygon
  under it. On large flat polygons v jumps at once by speed x the change of slope as a wheel crosses an edge (3
  degrees at 190 ft/s: 10 ft/s), and the damper throws the car or drops the wheel for that frame: the slight lifts
  over polygon edges that let cars spin out. Monterey's 1.6 ft curb at (-2870, -55, 8765), which its lanes cross at
  170 mph, has a 29 degree face 2.9 ft deep: a rear wheel that landed on it for a frame read v = -85 ft/s while its
  spring was extending, took 12.5 x the car's weight and flipped the car over backward; most computer cars flipped
  there. v may move only 200 ft/s per second (6.7 ft/s in a 1/30 s frame) past the larger of last frame's v and the
  spring's own rate in one frame; on smooth road it changes far less, so nothing changes, and a wheel that was off the
  road (a landing) keeps v. Tried: the spring's rate as the damper's input (a frame behind: the stiffer dampers, type
  39's, oscillated and the car hopped along at walking pace) and holding v to that slew over several frames (every
  car bouncier: 28 rollovers on Monterey against 13).
- **Tried and set aside** (kept in the file): `Hold::Copy` (California Speed's road hold as it is: weight x the
  per-wheel factors, square to the road or straight down once a wheel is more than 1.0 out, with the springs holding
  its ground part up: `rush2_calspeed_wheel_support`, hooked at 0x8006ADFC, records each wheel's support at
  0x34($sp) and the car is lifted by push x their sum / weight at its center, or each support is multiplied by
  (weight + push) / weight while a wheel is off the road. Per wheel all the time it made the springs 2.3 times as
  stiff in roll and pitch, and hooked at 0x8006AD18 it fed the tire load's cap, so a loaded wheel's tire jumped to the
  full weight and cars let go swerving downhill; lifted only at the center, cars landing on two wheels stayed tipped:
  California 90 wrecks and 140 rollovers against 62 and 47 per wheel). `Hold::Shape` (a push from the road's shape: weight x v^2 k / g from the
  pitch between the road under the front and rear wheels, averaged over 0.12 s; cars still lifted a little at
  polygon edges and spun), a push from the tires' unloading (the loads jump every step with the damper, so it
  snapped between 0 and 2 x weight on ordinary bumps), the whole road hold with the tires' load scaled back
  (`Hold::Full`: cars still handled differently and parked cars rolled 6-26 ft in the race intro), its air part alone
  over its ground part (`Hold::AirPull`) and air gravity (`Hold::AirGravity`, 3.6 x weight as the lowest wheel goes
  from 0.25 to 1.25 ft up): both start only once the car has begun to fly, so it lifted and was then yanked down.
- California Speed also stops cars from turning around on the track (they can only face along it); not located yet,
  and not ported.

### The subway tunnel loop at the end of San Francisco

The loop at the end of San Francisco is a subway tunnel (route points 360-440, polygons 7120-7288, surface type 0
material 6, spline flag 0x1060, type 6 walls at x 2446 and x 2561 up to y 658; the road runs y 14 at z -1109, y 476 at
z -2050, y 14 at z -3000, y 476 at z -3715, y 7 at z -4724). Under the Rush 2 engine the ride there is bumpy on
invisible collision, cars go flying and some are shot through the walls. tmp/cs_tunnel/race_log.py (a silent sim race
of computer cars that logs every car's position, velocity, compressions, surface types, polygon normals, attitude,
body rates and torque every 20 ms, then anomalies by route point) is the trace tool.

#### A missed wheel keeps its compression (tried, removed)

California Speed's ground contact `func_800A58A4` (Rush 2 `car_ground_contact_800703EC`) counts the wheels whose
probe found road; a wheel that found none keeps its last compression, travel and polygon when 3 or more did, and is put
1000 ft under the road (clamped to -3) only when fewer did. Rush 2 puts every missed wheel at -3 (its `0x800CA27C`
"no polygon" matrix) only above world y 197; below it a missed wheel reads +3 (see "Wall climbing" under CS Physics, fixed),
so one wheel probing past a polygon edge became an instant full unload or full load of that corner (a kick).
Ported as `rush2_calspeed_ground_contact_enter`/`_leave` (hooks at the entry of `car_ground_contact_800703EC` and at
0x80070680 before its wheel-speed clamp; src/calspeed/calspeed_walls.cpp): the four wheels' +0x5BC/+0x5CC/+0x5DC/
+0x5EC, type +0x5FC, halves +0x60C/+0x614/+0x61C, polygon matrix +0x2F0 and contact point +0x230 are saved on entry
and, with exactly 3 hits, the missed wheel's are restored (rate +0x5EC zeroed); a wheel that was already off the road
last step stays off. Fires about 55 times a race on San Francisco. Taken out of the build on 2026-10-18 with the wall
port (the user: collision broken in general); parked in tmp/cs_tunnel/removed/calspeed_walls.cpp. [V: disassembly; sim race]

#### Body rates: pitch and roll do not carry (tried, removed)

Two more California Speed rules were ported on 2026-10-18 and removed the same day because they broke driving on
hills. They are recorded so nobody ports them again. The orientation step `func_80090540` (Rush 2
`car_integrate_position_orientation_8006942C`) turns the car matrix +0x324 by its angular velocity +0x40 x dt
(`func_800C8A68`: yaw, pitch, then roll) and then zeroes the pitch and roll rates (+0x40 and +0x48 at
0x8009093C-0x80090944), keeping the yaw rate +0x44 (forced to +-5 or +-6.7 for the turning-around states 0x890). The
torque sum `func_8008FC1C` (Rush 2 `car_torque_from_forces_80069AB4`) adds, unless all four wheels are in the air
(surface type 10, Rush 2's 8), for every wheel whose compression is -0.1 or less a torque F x r with F = the car's
gravity +0x200 x -2 x (compression + 0.1) at the wheel's descriptor position (+0x74 there, Rush 2's +0x7C) with its y
zeroed (0x8008FDEC-0x8008FF5C), then its +0x134..+0x13C; no force goes with it. Both depend on the rest of
California Speed's physics (its road hold on the ground above all) and on Rush 2's they change how a car behaves on
every hill. Not ported either: `func_800901E0` (skipped once 0x8014FE0B is set) lifts the car along its up axis by
the mean of (+0x630 - 0.35) when positive and levels pitch when the player word +0x1dc4 is set; `func_8008FFFC`
clamps the car-frame velocity +0x34 to forward only (v.z >= 0), creeps it up to 6.25 and caps speed at 400;
`func_8008F7E4` turns the road hold's push to world down when any compression is below -1.0. The tick `func_8008F060`
runs `func_800A58A4`, `func_8008F23C`, `func_8008F0B4`, `func_8008FC1C`, `func_8008FFA8` via `func_80090BFC`,
`func_8008FFFC`, `func_80090540`. Trap met while porting: the MEM_ macros subtract 0xFFFFFFFF80000000, so a car
address taken from a register has to be `int32_t` (sign-extended); as `uint32_t` the writes land 4 GB past rdram and
the hook silently does nothing. [V]

Smooth collision (2026-10-18, src/smooth_collision.cpp, docs/custom_tracks.md#smooth-collision): on California Speed's
tracks every flat floor polygon's frame comes from a smooth normal (the blend of its corners' mean normals, folds past
20 degrees kept sharp), hooked in `collision_poly_extract_8006F704` at 0x8006FA70 after the surface dispatch; heights
stay the flat faces'. Spline polygons (flag 0x1000) keep their segment. The facets of the subway tunnel's curved floor
were the repeated kicks along the tunnel: with the smooth frame a 50 s silent sim drive from the user's spot past
checkpoint 3 through the tunnel (tmp/cs_tunnel/run.py ramp, bumps.py) went from 245 compression jumps over 0.5 ft,
219 vertical velocity jumps over 4 ft/s and 226 airborne samples to 31, 10 and 9.

The kick at the spot itself is the mesh, found with the RUSH2_SMOOTH_LOG=2 probe log and tmp/cs_tunnel/spot.py and
geom.py (which floor polygons hold a point, the leaf's polygons, their plane and spline heights): the road's right edge
there has a raised sidewalk, polygons 2727 and 5044 (type 0, 1.9 ft above the road along the shared edge, vertices at
the same x/z as the road's but 1.9 ft higher, no wall face), and a hairline crack (0.004 ft) between spline polygon
5064 and 2727 where a wheel finds no floor for a frame. The same pattern recurs along the whole course: sidewalk
polygons like 5019 over the road edge 2699 near (-503, -6786), and the subway platforms in the tunnel (2464/4766 at
-207 over the -209 floor near (-55, -5280)). The probe (and California Speed's own cs_wheel_probe 800AF1EC, same -5 to
250 window, nearest wins) takes the highest polygon holding the point, so a wheel point entering a sidewalk's footprint
is lifted 1.9 ft in one step. Fix, in the same file (hooks at 0x8006F748 and 0x8006F860 note the leaf's polygon count,
0x8006FBD0 fills a crack and continues at 0x8006FA70): a floor polygon within 1 ft of the point counts, the surface
nearest the wheel wins, a found surface more than 0.75 ft above the point gives way to the road while any is under the
wheel and otherwise is climbed at 0.1 ft per probe; neither applies to a car falling faster than 10 ft/s (car-frame velocity +0x38), so a landing from the hill road keeps its full depth and the floor holds it. Platform corners are then driven over as ledges (a climb and a
1.9 ft drop off the far edge) rather than launching the car.

Ramps (2026-10-22): the ledge rule also caught ramps. A car driving onto San Francisco's start ramps (the bridge
approach at (-1250, 10..40, 9700), the jump landing at (-1298, -4, 9340), the climb at (-1230, 76..133, 8030)) or
California's crest ramp at 150-250 ft/s puts a wheel more than 0.75 ft into the ramp in one step; held at 0.1 ft, the
suspension barely pushed, the ramp rose through the car and stood it nearly on end. Now each wheel keeps the spot it
last rested on (its point on the surface, from every probe not held as a ledge), and a found floor whose plane is no
more than 0.5 ft above that spot is a ramp and keeps its full depth. Neighbors by shared vertices can't tell the two
apart: San Francisco's continuous road polygons often miss each other's vertices by 1/32 ft, and some platform tops
taper to the road at their ends. Checked: test_race 67 with gas held climbs all of them upright (up >= 0.91, the
ramp's slope); the end-tunnel scenario's bumps match the earlier run and the ramp test never fires there.

Spline sags (2026-10-22): the car sank into San Francisco's road in places. Spline road polygons (flag 0x1000) hold
the wheel on the segment's cubic surface (`collision_seg_surface_8006BDA8`; California Speed's `cs_wheel_probe`
0x800AF1EC calls the same formula at 0x800AEE54, so this is the original data's behavior, not a port error). The
cubic meets each polygon at its corners and bows away between them, while the road drawn is the flat polygons (the
drawn mesh matches them within 0.03 ft). Where the road dips, the coarse triangles are chords over a sagging curve:
of SF's 854 spline polygons 124 sag more than 0.25 ft, 46 more than 0.5 ft and 15 more than 1 ft (worst 1.36 ft), at
the climb at x 2300-2550, z 1000-1980 (segments 67-83) and the hills at x 2370-2520, z -4520..-1800 (segments
363-429, polygons 7120-7288). On crests the cubic stands up to about 0.2 ft above the face instead. Smooth
collision's `spline_on_face` now holds a California Speed wheel on the higher of the cubic and the flat face (the
face's height measured along the cubic's normal), so the car never rides under the road it shows; crests keep the
cubic. Custom tracks' ROAD_SPLINES roads are left on the cubic, as their drawn road may follow it. Checked
(tmp/ledge_join/ride.py, tmp/cs_tunnel/run.py "sag climb" "sag hills"): over sags of 0.8-1.1 ft the car's body rides
0.27-0.36 ft over the drawn road (0.43 on flat road; on the cubic it would be about 0.7 ft under it), and through the
hills 0.51 ft over the face, 1.26 ft over the cubic.

Trap met on the way: `MEM_W(offset, base)` adds the offset to the (int32) base before widening to the 64-bit address,
so an offset held in a `uint32_t` constant makes the sum unsigned 32-bit and the access lands 4 GB away (reads zero,
writes vanish). The first smooth collision build read the POLY slot as 0, never matched the player's car, wrote its
frame into nowhere and its height from garbage, which was itself a source of 1.9 ft kicks. Stack slot offsets are
`int32_t` now.

### Rebalance

The California Speed cars' values are mapped onto Rush 2's roster as the 2049 cars' are (`Rebalance` in
src/calspeed/calspeed_cars.cpp; `tools/rush2049/cars.py calspeed` prints the three rosters side by side): each value
x Rush 2's mean (its 21 cars, all but the secret ROCKET) / California Speed's mean. Values every California Speed car
shares become Rush 2's mean: steering 10000 -> 543, yaw damping 1 -> 30.7, preload 0.05 -> 0.185, torque scales
1.9 / 1.95 / 2.05 -> 1.82 / 1.84 / 1.89, gears 3.05 / 1.68 / 1.17 / 0.88 -> Rush 2's, the tire curves (cornering
stiffness 64000 / 78000, grip 7.8) -> Rush 2's (16000 / 32000, 2.6 / 3.2). Values that differ by car keep their order
with the mean car on Rush 2's: springs and dampers (descriptor +0x18-+0x4C; mean -10620 / -8511 -> -3600 / -4200), mass
(140.4 -> 108.4 slugs), inertia (2614 -> 2729). The grip factors (+0x70, the Dozer 3.0) stay on top of Rush 2's
curves, and the handling row (California Speed's 4 for every car) is Rush 2's mean, 5. Inertia constants, +0x50 (16),
drag, rolling resistance, final drive, clutch and shift points are the same in both games. [V]

Rollover cap: a car rolls over rather than slides when its grip is high for how far its wheels sit from the middle
(every car's wheels are 0.8 ft below its center). Every Rush 2 car but the ROCKET and the DEW keeps grip / half track
at the Pickup's, 0.95 front (2.6 / 2.75) and 1.16 rear (3.2 / 2.75); 2049's cars are 2.5-3.75 ft wide on similar
curves. California Speed's cars are narrower (0.83 ft for the Golf Cart, 1.8 / 1.96 the Sportster, 2.16-2.45 most) and
its grip factors reach 3.0 (the Dozer), so on Rush 2's curves they sat at 1.06-3.86 and flipped at small steering
inputs. Each car's front and rear grip is held to at most the Pickup's ratio x its own half track (the rear with its
+0x78), so they slide where Rush 2's cars do (the Golf Cart 0.78 / 0.97, the Sportster 1.70 / 2.28, the 2.45 ft cars
2.32 / 2.85, the Dozer 2.34 / 3.10). The car select's bars count grip but not rollover, so before this a car could rate
well and still flip. [V]

### CS Physics: California Speed's world

The Games tab's **CS Physics** option (`calspeed_physics_mode` in games.json, 2026-10-21, user's design): Full CS
(this section), Partial (default) and Full Rush 2 (road hold off). Partial (2026-10-22) is Full CS with a player's car
on its own Rush 2 values and pitched level faster: the yaw gain, torque scales and gears, preload, mass, inertia,
springs, anti-roll, dampers, tire curves (no grip factors) and TIRES byte stay the car type's own (a California Speed
car's rebalanced onto Rush 2's roster), only the slide term (steer base +0x5A0) stays California Speed's; and the
levelling of a human's car (`orient_end`, func_800901E0's) runs at 2 rad/s instead of 0.15. The slide recorder
(`RUSH2_CSPHYS_SLIDES`) showed why California Speed's own steering spun a player's car: with the pitch rate zeroed every
step the levelling is all a car pitches by, so where San Francisco's hill steepens the front bottomed out (1.3 ft), the
lift held the rear wheels 0.5 ft off the road for up to half a second and the car swung around its front tires; the
computer cars don't steer hard there. With both, the user's drives had no fishtail. Test aids: `RUSH2_CSPHYS_RUSH=<letters>`
(y yaw gain, s steer base, c curves, d springs and dampers, e torque and gears, p preload, m mass, i inertia, t TIRES)
overrides the kept set, `RUSH2_CSPHYS_LEVEL=<rad/s>` the rate. Before 2026-10-22 Partial was Rush 2's physics with the
road hold above, which now runs only for mod tracks that ask for it. It
replaced the CS Road Hold toggle (`calspeed_road_hold`, kept unshown: off migrates once to Full Rush 2). The key
`calspeed_physics` held a 2026-10-19 test option whose `Rush2` meant the road hold, so the new option has its own key
and a leftover one is ignored. Every car runs native under Full CS, whoever drives it and whatever game it comes from
(2026-10-22): converted, a player's car fishtailed on every hill and landing; native it doesn't (the user's drives
2026-10-21 with a California Speed car, 2026-10-22 a Rush 2 car still converted did). Full CS is the other way round from the road hold port above: on California Speed's tracks the world runs as
California Speed's does, its computer cars drive its cars on its own values, and every other car is converted to that
world instead (src/calspeed/calspeed_physics.cpp, walls and missed wheel in src/calspeed/calspeed_walls.cpp). Added
2026-10-19 as a test: it replaces the default only if it races better (tools/calspeed/sim_wrecks.py `--hold cs`,
`--cs-drones`). Rush 2's wrecks, respawns and car-to-car collision stay for every car. Roles are set at car init, so
it takes effect from the next race; with it on, the CS Road Hold hooks stand aside.

California Speed's physics are Rush 2's functions with a few rules changed. Each CS function was lined up against its
Rush 2 counterpart with tools/calspeed/physdiff.py (struct accesses, constants, calls side by side; the car struct's
fields below +0x224 are at the same offsets, from there on shifted: +0x24, +0x58 from +0x2CC, +0x5C/+0x64 further) and
decompiled with `tools/rush2049/decomp.py cs` (m2c output in tmp/csphys/). The same code in both, so not ported: the AI
driver `func_8009AE0C` (Rush 2 `drone_driver_80074990`, same constants), its avoidance `func_8009CF00` (Rush 2
0x80071FBC), the difficulty tables (0.8 / 1 / 1.25 / 1.5 and 75 / 90 / 135 / 180), the steering terms
`func_8008F0B4`, the accel stage `func_8008FFA8`, the drivetrain `func_8008E1D0`, the wheel frame velocity and slip
functions and the tire model `func_800B8F68` but for its load cap. The physics step is `func_8008F060` (Rush 2
`car_physics_forces_800706D0`): ground contact `func_800A58A4`, wheel forces and force sum `func_8008F23C`
(`func_8008F268` + `func_8008F7E4`), steering `func_8008F0B4`, torque `func_8008FC1C`, accel `func_8008FFA8`,
velocities `func_8008FFFC`, orientation `func_80090540` (calling `func_800901E0`). [V: disassembly]

The rules that differ, as ported (Rush 2 fields; hooks in us.toml under "California Speed's physics"):

1. Road hold factor per wheel (`func_800A58A4`, see [Road hold](#road-hold)), kept from step to step, set after the
   wheel loop (0x80070680). Push = weight x the factors' sum along the car's down axis, or world down turned into the
   car's frame when any compression is below -1 (`func_8008F7E4`, after `car_force_sum` at 0x800706F4).
2. Hill push (`func_8008F7E4`): force z += 1200 when the forward axis y (+0x2E8) > 0.55, throttle +0x3B4 != 0, the rear
   wheels' road speed < 25 mph and no wall on body points 0 and 1 (`func_800B0A04`'s marks at CS +0x862).
3. The rear wheels' drive forces (+0x78, +0x84) always set to their mean (Rush 2 only for drive flag 1).
4. Lifted-wheel torque (`func_8008FC1C`, after the torque at 0x80070704): unless all four wheels are air (type 8), per
   wheel with compression <= -0.1, F = gravity +0x200 x -2 x (c + 0.1) at the descriptor wheel (+0x7C + 12i, y = 0).
5. Pitch and roll rates +0x40 / +0x48 zeroed after the rotation (`func_80090540` 0x8009093C).
6. Rest correction (`func_800901E0`) in place of Rush 2's lift by any wheel offset +0x5CC past 0.5 (held at 0.5 through
   `car_integrate_position_orientation_8006942C`): d = +0x5CC - 0.35 per wheel; for a player's car the pitch is first
   levelled (front z desc +0x84, rear +0x9C, at most dt x 0.15); if the sum is positive every offset drops by its mean
   and the car rises by it along its up axis. Wheel points +0x230 and body points +0x260 are then rebuilt.
7. Body points widened on the low side: |right axis y (+0x2D0)| > 0.3 gives w = 3 x (|r| - 0.3) signed; a body offset
   with w x x > 0 gets x += w.
8. Tunnel loop glue (`func_80090540`): with rear wheel materials +0x610 / +0x612 both 6, the car drops by its smallest
   travel +0x5DC if positive, and its matrix is rebuilt square to the last hit wheel's road normal (CS +0x3D8). The
   materials are California Speed's, kept per wheel in calspeed_physics.cpp (Rush 2's +0x60C holds the one its effects
   treat it as, 6 -> 0, src/surface_fx.cpp `source_material`): a lifted wheel gets 10 unless it was on 6.
9. Missed wheel keeps its compression with 3 hits, and the body-quad walls in place of the body probe (both from
   [Walls](#walls-california-speeds-body-collision) and [the missed wheel](#a-missed-wheel-keeps-its-compression-tried-removed),
   back in the build for this mode only).
10. Drag on dirt or grass under both rear wheels (`func_8008F268`): k = -1 x (1 - off-road), else -0.7 x off-road
    (Rush 2: -10 / -15, its track byte 0x8010D3A6 == 2 case aside); corrected after the wheel forces (0x800706EC) by
    (k_cs - k_r2) x v on force +0x54.
11. Grip factors: CS descriptor +0x6C (across) and +0x70 (along) scale every wheel's tire forces (`func_800B8CE8`; Rush 2
    only the rear lateral +0x78), and the tire load is capped at 1100 lb (`func_800B8F68`).
12. Velocities (`func_8008FFFC`, natives only, at the entry of `car_integrate_velocities_800697D0`): a creep to 6.25
    ft/s off the brake, speed capped at 400 (no spin integration that step), stop below 3 ft/s on the brake. Its
    forward-only clamp (v.z >= 0) is left out: the cars reverse with Rush 2's reverse gear (request -1, as Rush 2's
    `car_auto_shift_800711F4`).
13. Gearbox (`func_8008E5C4`, natives only, at the entry of `car_auto_shift_800711F4`): Rush 2's thresholds with road
    speed gates (up needs 26 / 66 / 106 mph out of gears 1-3; below 30 / 70 / 110 mph in gears 2-4 it shifts down);
    first gear picked by `func_8008E780`. Road speed = (spin x radius of rear tires 2 and 3) / 2 x 2.7273 (`func_800A3E20`).
14. Lane speeds: the lanes' true speeds are driven, without `capped_lane_speeds`. California Speed has no catch-up
    (its drones' speed and time factors +0x858, +0x454, +0x8AC are 1.0 all race); the port leaves it to the Drone
    Cheating option, as on every other track (forcing it off, the first build, overrode the player's setting).

Not ported: car-to-car collision (California Speed resolves every point contact, Rush 2 stops at the first), the
turnaround states, the x4 hold after the race, the brake-off-when-tipped rule.

Where Rush 2's rules meet California Speed's world (found in the sims, 2026-10-19):
- **Crash check.** Rush 2's `func_80071724` sets the crash flag +0x648 (and `func_8008E448` the wreck state +0x342 of
  the car's slot) when |force +0x4| (stored at +0x654) is above the threshold +0x644 (390000 lb for every car), when
  the speed +0x3D4 is past 400 ft/s, when the yaw rate +0x44 is past 10 rad/s, and when two body point forces (+0xB8
  y) are below -1000 with the car tipped past 45 degrees under 100 ft/s (one below -30000 only adds damage bits).
  California Speed has none of it: its threshold table 0x8007D134 is never read. In this world every car is held down
  with 2.28 g, and computer cars landed California's crest (1030, 70, 7450) at 240 ft/s with 400000-585000 lb,
  upright on their lane, every lap. So every car's threshold is x 2.28 at its init. California Speed stores the speed
  before its 400 ft/s cap; here the capped speed is stored, or every car falling down San Francisco's tunnel hump at
  the cap was wrecked.
- **Materials.** Rush 2's +0x60C holds the material Rush 2's effects treat a California Speed material as (6 -> 0,
  src/surface_fx.cpp), so the tunnel glue reads California Speed's own, kept per wheel (`source_material`). Before
  that the glue never ran: computer cars left the hump's crest at 170 ft/s, flew 4 s and came down through the
  tunnel floor.
- **Smooth collision** (src/smooth_collision.cpp) runs here too since 2026-10-21 (the user's request: without it the
  player's car got the same crack and ledge kicks as the old physics before smooth collision). It stood aside before
  because its ledge rule (a floor more than 0.75 ft above the wheel gives way to the road under it, climbed at 0.1 ft
  a probe) let cars at California Speed's 250 ft/s sink through the ramp up to California's crest onto the ground
  under it and fall off its end; check that ramp when testing this mode. Since 2026-10-22 a floor rising from where
  the wheel last rested counts as a ramp, not a ledge (see "Ramps" under the subway tunnel fix). California Speed's own probe takes the
  highest floor holding the point, as Rush 2's.
- **Resets.** Rush 2's `race_stuck_wrongway_update_8008CDA4` resets a computer car (wreck state 2) with the flag
  +0x810, below the fall floor, or under 25 ft/s for 6 s (12 s for some); these stay. Most of the remaining Santa
  Cruz wrecks are these, around its lane's 213 -> 117 mph step at (-18000, -690, 5700).

Test aids: RUSH2_CSPHYS_LOG=<physics car> prints that car's wheels on the tunnel (materials, compression, travel,
hold factor, normal) and its drivetrain every 15 steps; RUSH2_CSPHYS_WRECKS=1 prints each wreck with the crash
check's inputs and every `car_wreck_trigger_8006E1A8` call. tools/calspeed/sim_wrecks.py `--hold cs` races in this
mode, `--cs-drones` with only California Speed computer cars, `--where` lists each wreck, `--read` re-reads the
recordings.

Roles (`rush2_csphys_init_begin` at the entry of `car_init_setup_8008DBA0`, the tables put back at 0x8008E01C):
- **Native**: every car. A California Speed car: the descriptor pointer for the init points at its race
  copy (0x80225000 + slot x 0xEC), built unscaled by `rush2::calspeed::native_car` with California Speed's own tire
  curves at 0x80225C00 + slot x 0x30; steer, yaw damping, preload, mass (x 0x8007D47C), inertia and TIRES 4 come from
  California Speed's tables (0x8007CF20, 0x8007CF74, 0x8007D034, 0x8007D088, 0x8007D0DC). Any other car (Rush 2,
  2049, mod): `rush2::calspeed::native_values` undoes the California Speed cars' mapping onto Rush 2's roster
  ([Rebalance](#rebalance)) on its own values, so a Rush 2 car stands to California Speed's as it does to Rush 2's: the
  descriptor's springs, dampers, torque scales and gears / the mapping's ratios, its tire curves / theirs (Rush 2's
  two become California Speed's exactly), yaw damping, preload, mass and inertia likewise, TIRES 4, grip factors 1
  (its rear grip +0x78 stays). The steer base is moved by the two means' difference (California Speed's 10000 against
  Rush 2's 543) instead of scaled, so the cars with 0 (SLED, BUGATTI, CONCEPT, GT90) keep the slide term. With a
  player's options the descriptor copy then gets `rush2_car49_setup_desc`'s changes (slicks, 2049 ENGINE, Car
  Speeds) as on any track. Sims (2026-10-22, tracks 64-66): the Rush 2 and 2049 computer cars had no wreck, rollover or
  slide.
- **Converted**: the fallback when the California Speed cars aren't loaded: its own values, its
  descriptor copy's springs and anti-roll (+0x18-+0x2C) x 2.28 and dampers (+0x30-+0x4C) x sqrt 2.28, the tire load /
  2.28 (no 1100 cap): the world holds it down at 1 + 4 x 0.32 = 2.28 g on the road, so it sits, rolls and grips as at
  Rush 2's 1 g. It keeps Rush 2's velocities (reverse) and gearbox, and Rush 2's own lift (any wheel offset past
  0.5 ft) in place of `func_800901E0` (the lift by the four offsets' mean past 0.35 ft and a human's pitch levelling).
  That lift took load off all four tires at once, and a converted car's load is uncapped, so its light rear let go:
  the user's fishtails on San Francisco's downhill (2026-10-21). A native's load (about 2580 lb a wheel) sits over its
  1100 lb cap, so it never felt it. San Francisco sims, slides past 25 degrees at over 60 ft/s (sim_wrecks.py counts
  them): converted cars 16 with it, 5-10 without, 4 on Rush 2's physics; native cars 0.

Test aid: `RUSH2_CSPHYS_SKIP=<letters>` leaves California Speed rules out for converted cars (r pitch/roll rate
zeroing, a rear drive force averaging, h road hold, u hill push, t lifted-wheel torque, d drag term), to find which
one a behavior comes from. On San Francisco: a and u change nothing, t is noise, r and h make slides worse.
`RUSH2_CSPHYS_SLIDES=<file>` records a player's converted car: whenever it starts to slide (more than 15 degrees off
its heading above 40 ft/s) the 240 physics steps before and 120 after go to the file (steering, throttle, brake, yaw
rate, slip, and per wheel compression, surface/material, tire load, forces along and across). The sims can't drive
a player's car, and the user's swerves came from steering the computer cars never do.

Tire load cap: Rush 2 caps a wheel's load at the car's weight (`car_wheel_force_loop_8006ABB0` at 0x8006AD30; the
uncapped load stays at 0x34($sp)) before the converted car's / 2.28, so its tires topped out at 0.44 of its weight a
wheel (0.25 at rest). A converted car's is capped at weight x 2.28 instead (2026-10-21), Rush 2's cap after the scale.

#### Wall climbing: a missed wheel reads as fully compressed (fixed 2026-10-27)

Report: with CS Physics, cars drive up walls (video: Yosemite, a left-hander whose outside is the 320 ft type 5 wall,
polygons 329-334, x -2006..-1755, z 394..588, y -904..-580, its foot on road polygons 1016-1022 at y -904..-936).
Reproduced in a sim (Partial, car 0 put on the road there at 235 ft/s, 25 degrees into the wall): the car climbs from
y -904 to past +160 in 3 s. Cause, from the per-step log:

- Rush 2's wheel probe `collision_poly_extract_8006F704` has no "air" result. When no polygon holds the wheel's point
  (0x8006FBD0) it stores surface type 1, material 1, the identity frame `0x800CA27C` and a height of the wheel's
  world y - 200 (0x8006FBFC-0x8006FC10). `car_ground_contact_800703EC` takes compression = offset +0x5CC - height
  (0x8007058C), clamps it to +-3, and sets type 8 only when it is below 0. So a missed wheel is airborne (-3, type 8)
  only above world y 197; below that it is fully compressed (+3) and stays type 1 (dirt). Every California Speed track
  is 89-100% below y 197 (Yosemite -1000..444), and so are Rush 2's own (82-100%), so this is stock Rush 2 behavior
  that its closed tracks rarely reach.
- California Speed's ground contact `func_800A58A4` handles a miss itself (its probe returns 0): -1000 clamped to -3
  with fewer than 3 hits, last step's values with 3. The port assumes Rush 2's miss is already -3/type 8 (the comment
  in src/calspeed/calspeed_walls.cpp and the section above), so on these tracks `keep_missed_wheels` never fires (it
  looks for type 8 and -3) and a car with 2 wheels past a wall's plane gets +3 on both instead of -3.
- Against the wall the two wall-side wheels poke past the wall's plane (0.3 ft behind it), where no floor polygon holds
  them: +3 each, so Rush 2's springs at full compression push along the car's up axis, the road hold factor of a
  wheel past 0.5 ft (0.82 - compression = -2.18) turns the hold into about 8 g along the car's up axis, and the tires
  get full load on "dirt" (the dust in the video). Rolled against the wall, the car's up axis points up and out, so
  it rides up the face. Smooth collision's crack fill is not the cause (RUSH2_SMOOTH_COLLISION=0 climbs the same).

Also found while comparing `func_800B0A04` with `apply_hit` (src/calspeed/calspeed_walls.cpp): California Speed's push
out of a wall is 2.5 x 1/dt x (1 - |forward . normal|) (`lui 0x4020` at 0x800B0D20); the port used 8.

Fix (src/calspeed/calspeed_walls.cpp, both CS Physics modes): the probe's miss path notes the wheel
(`rush2_calspeed_probe_miss`, the hook at 0x8006FBD0 after smooth collision's gap fill), and after the wheel loop
California Speed's rule is applied to those wheels: with 3 hits the missed wheel keeps last step's values (unless it
was already off the road), otherwise it is off the road as Rush 2 has a wheel with no road under it (compression -3,
travel 1000, offset 0, type and material 8, travel speed from the change clamped to 40). The wall push is 2.5. Full
Rush 2 is unchanged (stock Rush 2 behavior).

Test aids: `RUSH2_CS_PLACE="x,y,z,dx,dz,speed,at"` puts the player's car (physics car 0) at a spot, heading (dx, dz)
at speed ft/s, once its clock passes `at`, and logs it each step for 8 s ([csplace]; src/calspeed/calspeed_physics.cpp).
`RUSH2_CS_OLD_MISS=1` runs the port as it was before the fix. The report's spot:
`tools/test_race.py 74 --games calspeed_physics_mode=Partial --env RUSH2_CS_PLACE=-1874,-904,470,0.502,-0.863,235,1`.
Results there (highest y reached from the start at -904): before the fix +160 (Partial) and +248 (with
RUSH2_CS_OLD_MISS); fixed, -904 in Partial and Full CS at 25 degrees and 160 mph, and in Partial at 10 and 45 degrees
and at 45 degrees and 20 mph (the car slides along the wall, or stops against it, and drives on). Computer-car sims
(`sim_wrecks.py --hold on`, San Francisco, Santa Cruz, Yosemite), old against fixed: wrecks 1/1, 0/0, 0/0, rollovers
0 in all, slides 2/5, 6/1, 5/2, every car finished.

### Brake lights turn California Speed grays red

User report (2026-10-20): white and silver parts of California Speed cars (SEMI's exhaust stacks, CAMPER, OL TRUCK,
DOZER...) are red in a race but right on the car select. [V] (RAM dump at race start, `tmp/cs_red`.)

Rush 2's brake light update `func_800599A0` (registry name `carselect_update_preview`, a misnomer; it runs for every
race car, `$a0` = car callback entry with the car index at +0xE, `$a1` = brake lit) rewrites entries 161-175 of the
car's palette copy (car palette record +0x14, `+0x142`-`+0x15E`) every frame unless 0x800E7BB0 is set: lit, all
0xF801 (bright red); unlit, copied back from Rush 2's base CARPALETTE (0x80119648 + 0x142, its red tail light ramp).
Lit needs `$a1` set and the car's flags (0x80112584 + car x 0x354) to have 0x1000 and not 0x10 (0x80059A1C). In Rush
2's CARPALETTE 161-175 are the tail lights; in California Speed's CARPALETTE (asset 25) 152-175 is the gray ramp its
cars use for white, silver and chrome (SEMI's TRUCK_4 is all grays, CAMERO's headlight), so every such texel of
161-175 showed Rush 2's tail light red. The car select doesn't run it, hence the difference. `rush2_car49_paint` paints
California Speed's palette correctly; this later per-frame write is what changed it.

California Speed's own tail lights are CARPALETTE's dark red ramp 128-143 (CAMERO's CAM_7, DIABLO's DIABLO_3/4). No
California Speed code was found writing car palette entries at run time (no 0xF801 store but its traffic light
animation at 0x800B989C, nothing at the palette's 128-143 offsets), so the lights are Rush 2's rule applied to these
entries. The same reds are also drawn by the driver figures (MAN, ZWOMAN, WCHIC, SPR_GUY) and other art, so a palette
write would flash them too: the lit state is a texture swap instead. `add_damage_textures` gives each body texture
(texture record flags 0x20000000, which only the car's panel textures have) that holds 128-143 a lit copy, those
texels as entry 187 (0xF801, Rush 2's lit red), and one of its scuffed copy. `rush2_calspeed_brake_lights` (hook at
0x800599A0) skips Rush 2's write for a California Speed car and keeps its lit state by Rush 2's rule;
`rush2_car49_dent` points the body's texture loads at the lit copies while it holds.

### Car name art

California Speed has no name logos for its own cars. Its car select bank, asset 9, is Rush 2's (asset 0x11): the same
26 textures, among them Rush 2's 22 car logos (4X4, BANDIT, COMPACT, CONCEPT, COUPE, CRUISER, DEW "Mtn Dew", EXOTIC,
FORMULA, HATCHBACK, HOTROD, MOBSTER, MUSCLE "Muscle Car", PICKUP, PROTOTYPE, ROCKET, SEDAN, SPORTSTER, STALLION,
SUBCOMPACT, TAXI, VAN; Rush 2's type -> logo table is 0x803C7688), every one redrawn in California Speed's lettering:
a bold pixel sans, mixed case, in a blue gradient (palette 1-11, light at the top) with a dark edge (12-15), all but
HOTROD on one shared palette, rows stored bottom-up. Capitals and l span rows 5-23, the x-height starts at row 10,
descenders reach row 27. Only Sportster, Muscle and Mt Dew among its cars have a word. [V]

The port (`src/calspeed/calspeed_logos.cpp`, cached as the blob `calspeed_name_logos`) uses those three words as they
are and spells the other 18 names (486SE, Predator, Mano, Five Oh, Hopper, Fairchild, Insect, Mercado, Golf Cart,
Squirrel, Forklift, Conv, Sled, Baja, Ol Truck, Camper, Semi, Dozer) from letters cut out of the words: a letter is the
pixels in a column window of a word whose 4-connected piece inside the window holds fill, so a touching neighbor's
edge stays behind (the T and V also drop a neighbor's corner piece). Sources: P PICKUP 25-39, M MOBSTER 18-35,
F FORMULA 17-29, H HATCHBACK 4-18, C MUSCLE 87-103, S SEDAN 27-41, E EXOTIC 29-41, D DEW 66-80, B BANDIT 27-42,
T TAXI 41-54, 4 4X4 44-57, V VAN 42-59, a SEDAN 71-83, c MUSCLE 48-59, d SEDAN 57-69, e MUSCLE 67-79, h HATCHBACK
56-67, i CRUISER 65-68, k HATCHBACK 112-123, l MUSCLE 62-65, m COMPACT 46-64, n SEDAN 86-97, o COMPACT 31-44,
p PICKUP 89-102, r CRUISER 41-48, s MUSCLE 34-46, t BANDIT 93-100, u MUSCLE 20-32. The letters no word uses are put
together from real ones: I = l; O = C united with its mirror image, 8 = S with its mirror; 6 = the S's top (its right
side cut after row 8) on the 8's bottom loop; G = C closed by the mirrored C's lower right and a bar; q = p mirrored;
f = r's arm raised to cap height on an l stem with t's crossbar; j = i continued by p's descender with a hook; v = V
resampled to the x-height at 0.78 width; z is drawn, colored with the lowercase letters' median fill per row. Letters
keep their rows and are set one texel apart (a space is 6). If the bank isn't as expected the names fall back to the
port's 5x7 lettering. [V in game]

## 8. Open items

- The menu song (for a California Speed menu song switch in the Sound tab).
- What the finer route line in the paths is for.
