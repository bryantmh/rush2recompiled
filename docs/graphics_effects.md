# Graphics effects: ambient occlusion, car reflections and car light probes

Renderer effects the original game doesn't have, in Settings > Graphics > Effects and switchable while the game
runs. It is a change to RT64, kept in `lib/patches/rt64.patch` (edit under `lib/rt64`, then
`python tools/lib_patch.py save rt64`).

| Option | Values | Default | RT64 global |
|---|---|---|---|
| Ambient Occlusion (`ambient_occlusion`) | Off, Low, Medium, High = strength 0, 0.35, 0.6, 0.85 | Off | `RT64::AmbientOcclusionStrength` |
| Reflection Blur (`car_reflection_blur`) | 0% to 100%, steps of 5 | 20% (0% is a mirror) | `RT64::CarReflectionBlur` (0 to 1) |
| Reflection Rate (`car_reflection_rate`) | 30, 60, Every Frame | 30 | `RT64::CarReflectionUpdatesPerSecond` (30, 60, 0 = every frame) |
| Car Sheen (`car_sheen`) | Original, All Off, All On, Cube Map | Original | none: under Cube Map the game marks draws with `gEXSetCarReflection` |
| Car Reflections (`car_reflections`) | 5% to 100%, steps of 5 | 70% | `RT64::CarReflectionStrength` (0 to 1) |
| Car Lighting (`car_lighting`) | Original, Light Probes | Original | none: game side only (`rush2::probes::set_mode`, [Car light probes](#car-light-probes)) |

The option lives in `src/config.cpp` (`add_ambient_occlusion_option`). The global is
declared in `lib/rt64/src/common/rt64_enhancement_configuration.h` and read every frame by
`FramebufferRenderer` (`lib/rt64/src/render/rt64_framebuffer_renderer.cpp`).

## Depth to distance

The effect turns depth buffer values back into distances. For each perspective projection, the renderer works this
out from the projection's matrix and viewport (`drawData.viewProjTransforms` and `drawData.rspViewports`, the same
ones `RSPProcessCS.hlsl` transforms vertices with):

- The view only moves and turns the scene, so a projection's clip z is `c * w + e` for constants c and e. They come
  from the matrix's z and w columns (row vectors, `mul(v, M)`): c = M[i][2] / M[i][3] for the row i with the largest
  M[i][3], and e = M[3][2] - c * M[3][3].
- The depth buffer holds `translate.z + scale.z * clip.z / w`. So depth d gives `w = depthB / (d - depthA)`, with
  `depthA = translate.z + scale.z * c` and `depthB = scale.z * e`.
- w is in the game's view units. Rush 2's are its vertex units, 1/16 of a foot: its near plane (2.5 ft) is w = 40, and
  depth never reaches 1 (d tends to depthA, about 0.9987, far away). A cleared pixel (d about 1) gives a negative w,
  which is treated as background.
- The distances in `rt64_framebuffer_renderer.cpp` (`AmbientOcclusionRadius`, `AmbientOcclusionFadeStart`,
  `AmbientOcclusionFadeEnd`) are in these units, written as feet x 16.

## Ambient occlusion

Screen space ambient occlusion, two full-screen passes per run of 3D draws:

1. `AmbientOcclusionPS.hlsl` (and `AmbientOcclusionPSMS`, for a multisampled depth buffer, which reads sample 0)
   reads the depth buffer into a half-float two-channel target the size of the color target (made on first use and
   kept in the framebuffer storage, `RenderFramebufferStorage::occlusionTexture`). Per pixel it rebuilds the view
   position from w and the pixel (`AmbientOcclusionCB.focal` = pixels per unit of y / w, the ratio of the matrix's y
   and w column lengths times half the viewport height; `center` = the viewport's center), takes the surface normal
   from the nearer neighbor on each axis, and tests 12 points on a disc of 6 ft. Each point above the surface adds
   `max(cos - 0.25, 0) * (1 - d^2 / r^2)`; the 0.25 leaves gentle folds (two slopes of a road) alone. The disc turns by
   the pixel's place in a 4x4 ordered dither tile. Output: the ambient (1 open) and w. Occlusion fades out from 200 ft
   to 400 ft.
2. `AmbientOcclusionApplyPS.hlsl` averages the first pass over 4x4 pixels, which holds each turn once, skipping
   pixels whose w differs by more than 4%, and the pipeline multiplies it into the color target's RGB (blend
   ZERO / SRC_COLOR, alpha untouched).

When it runs (`submitRasterScene`): every opaque, depth-writing draw with a perspective projection
(`triangles.occlusionIndex >= 0`) marks occlusion as pending and grows the screen rectangle it covers. The passes run
before the next 2D draw (rectangle, raw triangles, a non-perspective projection), clear, or the end of the scene, so
the HUD drawn after the 3D isn't shaded, and in split screen each view's run is shaded once, over its own area.
Blended draws drawn before that point (smoke, glass) are shaded by what's behind them.

## Car reflections

Settings > Graphics > Car Sheen = **Cube Map**: during a race every car's paint reflects the scene around it
instead of sliding a sheen texture over it. Car Reflections (strength), Reflection Blur and Reflection Rate adjust
it and are grayed out under the other Car Sheen modes (`car_sheen_option::apply` in `src/config.cpp`). All
hot-swappable (the game decides per draw, RT64 reads the settings per draw). The car select and the shop draw as
All On, with the sheen textures: there is no scene around the car to reflect.

**Game side** (`sheen_model` in `src/rush2049/car2049.cpp`, docs/rush2049_research/cars.md#car-reflections): each SHEEN
layer node (car record +0x50..+0x60 and the hood's +0xC / +0x10) has its display list wrapped in
`gEXSetCarReflection(car + 1)` ... `gEXSetCarReflection(0)`, and the DECAL layer nodes (+0x3C..+0x4C) draw nothing.
The layers are copies of the panels' triangles drawn over them, so they are exactly the paint, already hidden with
the car, dented with it and swapped with its damage stages.

**RT64 side** (`lib/patches/rt64.patch`): `G_EX_SETCARREFLECTION_V1` (0x35) sets `DrawExtendedFlags::carReflection`,
which splits draw calls like the other extended flags. Then, per frame:

1. `addFramebuffer` sorts each perspective projection's indexed draws into marked ones and the rest (the ones a cube
   map can redraw), and keeps the last fill color before each projection. `addCarReflectionScenes` makes one
   `CarReflectionScene` per view: the projections with the same view projection (`modViewProjTransforms`, what the GPU
   used) and viewport. It works out each marked car's bounds from the marked draws' vertices
   (`posFloats` x `lerpWorldTransforms`, the space the world transforms put vertices in) and centers the cube on the
   car nearest the camera. The camera is where clip x, y and w are 0. Every marked car in the view reflects that one
   cube, so AI cars further away show the player car's surroundings.
   It also gives each car's marked vertices smooth normals, on the CPU, into the slot's upload buffer
   (`CarReflectionSlot::normBuffer`, indexed like the frame's vertices): the layers' vertex colors are white with
   no normals. A car's panels are separate draws, so the normals are worked out over all of a car's marked
   triangles at once (the group number tells the cars apart): vertices within 1% of the car's half size are one
   point, and each point sums the normals of the triangles around it weighted by their angle there. Face normals
   take their side from the triangles' winding, turned around for parts drawn with a mirroring world transform
   (negative determinant), so they agree across the car, concave parts included; the pixel shader works out which
   side faces the camera. (Turning them away from the car's center instead was wrong for inward-facing surfaces,
   the inside of a fin.) Normals per draw left a crease at every panel edge, which made the cars look flat shaded.
   The scene also keeps each car's bounds in the cube's space (`CarReflectionScene::carBoxes`,
   `triangles.reflectionCar`) for the self-reflection fade below.
2. At the view's first marked draw, `submitCarReflection` draws the cube, at most
   `RT64::CarReflectionUpdatesPerSecond` (Reflection Rate) times a second per view: the atlas is kept in the scene's slot
   (`CarReflectionSlot`, one per view, with the mirroring it was drawn with) and the frames in between, mostly the
   display's extra frames, reflect the last one drawn. The compute pass still runs every frame for the marked
   draws' positions, but only writes the faces' positions when the cube is due (`CarReflectionCB::writeFaces`).
   - `CarReflectionCS.hlsl` puts every vertex back in the cube's space from its screen position. It inverts only the
     view projection's x, y and w columns (`CarReflectionCB::invClip`, the car's center folded in), so the depth's
     precision loss doesn't matter and anything the RSP did to the vertex (interpolation, `G_MODIFYVTX`) is taken in.
     It then projects each vertex to the six faces: a 90 degree perspective with an infinite far plane (depth
     `1 - near / w`), written as screen positions the raster vertex shader turns back into clip space with
     `FramebufferParams::resolution`. Each face's near plane is the car's half size on that axis plus 5%, so the
     car's own body is clipped out of its reflection; the wheels still show in the lower faces.
   - The view's own draw calls, with their own pipelines, textures and render params, draw each face of a 768 x 512
     atlas (six 256 px faces, 3 x 2) that has the color target's format and sample count, so those pipelines fit it.
     The atlas is cleared to the view's fill color (the sky or fog color, for what the draws leave empty) and
     resolved when multisampled. Decals in the faces test against the dummy depth buffer, so they may be missing.
   - The faces mirror (`CarReflectionCB::mirror`) when the view projection's x, y, w part, counting the viewport's
     flips, has the other handedness, so triangles keep their winding and back faces stay culled.
3. Each marked draw is drawn by `drawCarReflection` (`CarReflectionVS.hlsl`, `CarReflectionPS.hlsl`) instead of its
   own combiner: the view off the smooth normal into the cube (face by the largest axis, the face layout in
   `CarReflection.hlsli`), blended over the paint with Fresnel alpha `0.12 + 0.88 (1 - cos)^5` x the strength, depth
   tested `LESS_EQUAL` (nudged 1e-6 towards the camera) against the paint it lies on, no depth write. With
   Reflection Blur above 0 the color is the average of 13 directions in a cone around the reflected one (the center
   and two rings of six, at tangents 0.14 and 0.28 times the blur), glossy rather than a mirror, which hides how
   few polygons the cars have; at 0 it is the one direction. The smooth normal is given the side of the polygon's own normal that faces the camera, and where it
   still leans away from the camera (near the car's outline) it is tipped back to the outline rather than flipped.
   Self-reflection: the cube has no car in it, so a reflection that would hit the car itself (the inside of a fin,
   the hood towards the windshield) showed the road on the car's far side. The shader measures how far the
   reflected ray runs inside the car's bounds (`CarReflectionDrawCB::carMin` / `carMax`) and fades the reflection
   out from 0.3 to 0.8 of the car's largest half size. The bounds are a loose fit (a fin's inner face reflecting
   forwards and up leaves them above the trunk long before the cabin), so the reflection also fades as the
   polygon's own normal turns towards the car's center (cosine 0.05 to 0.45): such a surface mostly sees the car.

Limits: the cube only has what the view drew, so scenery the game itself left out (its PVS by camera region, LOD
distances) is missing; RT64 doesn't run `G_CULLDL`, so the placement box culls don't remove anything; draws in
other projections (a sky drawn with its own projection) are left out; one cube per view. Cost per view with marked
cars: a compute pass over the frame's vertices, the view's draws six more times at 256 px, and the normals on the
CPU. Measured 2026-10-13 on Las Vegas, one player, eight cars, at the 172 fps cap: GPU load about 39% without
reflections and 49% with the cube drawn every frame (2560 x 1415), 42 to 47% with it drawn 30 times a second (that
run was at 3440 x 1411, so the saving is understated). Split screen draws one cube per view.

Checked in game 2026-10-13 on D3D12 (single player race). Still to look at closely: inverted faces (mirror sign),
the car showing in its own reflection (near planes), and the sky in the up face.

The reflection draw pipeline's layout needs `layoutBuilder.begin(false, true)` (input layout allowed), like the
raster pipelines: without it D3D12 fails to make the pipeline and the first marked draw crashes in `setPipeline`.

## Car light probes

Settings > Graphics > Car Lighting = **Light Probes**: each side of a car takes the light of what is around it, a
cheap stand-in for global illumination. Cars darken under bridges and in tunnels, pick up the color of walls and lamps
beside them and brighten toward open sky. Original keeps every game's own car lighting (Rush 2's tunnel darkening,
Rush 2049's painted colors, collision.md#car-lighting). Game side only, no RT64 change; decided per draw, so it switches
mid-race.

**Bake** (`src/car_probes_bake.cpp`, `include/car_probes.h`; nothing of the game, so `tools/car_probes_test.cpp` runs it
on Rush 2's twelve stock tracks and prints their spread):
- The world is the track's models as its placement file places them (`read_container` / `read_placement` of
  `src/asset_catalog_walk.cpp`): world = vertex x matrix / 16 + position, rows as row vectors. Placement names match
  model names as `write_glb` matches them (exact, else the shortest name starting with it), in the track container
  first, then the shared containers 0x12 and 0x14. Meshes whose texture is mostly see-through (average alpha < 0.6:
  tree and fence cutouts) are left out.
- Each triangle's light = its average vertex color x its texture's average color x 1.6 (bounce gain: without it
  tunnels came out nearly black). Front faces are clockwise in the game's left-handed space, so a front normal is
  e2 x e1 (turned for a mirroring placement).
- Probes: one per 5 ft cell, 2.5 ft above upward-facing triangles (front normal y > 0.7). Triangles over 20,000 sq ft
  (the sea, backdrop hills, arena floors, long stretches of road) get them on a 20 ft lattice instead: at full density
  they held most probes (Hawaii 480,000 of 580,000), and with none (the first version) a car on them had no probe in
  reach and was lit neutral, a white flash between the probes around it (6% of NY Two's AI laps, 42nd Street). The
  lattice probes are left out of the open-road reference below, or the sea and fields pulled it down.
- 256 rays per probe (a Fibonacci sphere, turned at random per probe so neighbors don't share blind spots) through a
  median-split BVH; a miss, or a hit past 400 ft, is white sky (the sky counts wherever it is in view; there is no
  sun, so no sun shadows: on 2049 tracks those are the artists' painted colors). A probe whose rays see mostly back
  faces is inside something (a deck, a wall) and dropped. Each of the six cube faces (+x, -x, up, down, +z, -z) is
  the cosine-weighted average of its hemisphere's rays, but the four sides take only the rays level with the car or
  above it: the ground's light is the underside's alone. With the ground in the sides, grass against road changed the
  hood, roof edges and deck (whose normals slope sideways), and probe-to-probe ground differences flickered there.
- Each probe is then averaged with its neighbors within 12 ft (height 3x, weights (1 - d^2 / r^2)^2,
  `BakeParams::smooth`), which halves the difference between neighbors (Marina p95 0.33 to 0.13).
- Each face is divided by the median luminance of that face over the probes open to the sky above (90% of the upward
  rays miss), so an open road is 1 on every side; tracks with under 5% of those (PIPE, HALFPIPE) use the median over
  all probes. Faces are kept in 0.25 to 1.0: no side is lit brighter than an open road (with 1.4 allowed, a lamp-lit
  tunnel floor lit cars from underneath, which looked like a glow).
- Stock tracks: 52,000 to 800,000 probes, 0.5 to 6 s on the worker thread (once; cached), 1.5 to 22 MB. Covered spots come out at 0.45 to 0.6 from above,
  close to Rush 2's own tunnel darkening.
- Rush 2049 tracks: each probe also keeps the painted lighting color of the 2049 collision polygon straight under it
  (`rush2::track2049::car_light_paint`: polygons with a color index other than 0, not flagged 0x30, not movers, colored
  from the track's table by `car_light_table`; docs/rush2049_research/collision.md#car-lighting), 4 bytes a probe.
- Storage: sorted 21-bit cell keys, 3 bytes for the probe's place in its cell, 18 bytes of light (x 128), then the
  paint. `ProbeSet::sample` blends the probes within a radius (height counting 3x) weighted (1 - d^2 / r^2)^2, falling
  smoothly to 0, and the paint the same way over its own radius. With none in reach (a car in the air) it takes the
  probes up to 80 ft under the car, height counting a quarter.

**Game side** (`src/car_probes.cpp`, called from `rush2::car2049::light_model`, `src/rush2049/car2049.cpp`):
- The raced track: the geometry slot record 0x8010C400 indexes the asset slot records at 0x8010C258 (12 bytes: u8
  asset, u8 model slot, u8 mirror, ptr data); its asset is 0x33 + slot. The bake reads that asset, 0x3F + slot and the
  shared containers as the game loads them (`rush2::assets::read_current`: stock, SF Rush, Rush 2049 and custom tracks
  alike) on a worker thread, and keeps the result as a `track_cache` blob named after the hash of those bytes and
  `bake_version`. Until it is ready the cars draw as Original.
- Each race car's cube (`rush2::probes::CarLight`, in the bake file so the replay tool runs the same code) follows the
  probes at its body node's world position (node +0x4 transform, +0x24 translation): sampled over a 20 ft radius,
  0.06 s of the car's motion ahead (at most 30 ft), and eased toward with a 0.08 s time constant; a jump of over 60 ft
  (a respawn) takes the new light at once. The look-ahead cancels the easing's lag. History: the first version sampled
  nearly the nearest probe and eased over 0.15 s with no look-ahead (reported as late and as stepping); a 35 ft /
  0.35 s / 0.25 s version was smooth but far slower than Rush 2049's own lighting, which changes the instant a wheel
  rolls onto a differently painted polygon (Presidio's lamp pools at the start). Tuned with `car_probes_test --path`
  on AI paths recorded on NY One and NY Two (`RUSH2_PROBE_TEST=path`, 10,800 records each): the settings above match
  the light at the car with no time shift (within the records' 0.05 s), largest change in 50 ms 0.30 / 0.35 (light of
  an open road = 1).
- The cube drawn (`CarLight::lit`) is the eased one softened (each face moved 40% of the way to the mean of every face
  but the underside, so the light turns gradually from roof to underside instead of meeting in a line along the
  beltline, and the ground never pulls the roof), then painted.
- On a Rush 2049 track, where the artists painted a lighting color, the painted share of the cube takes the color's tint
  and brightness, keeping the cube's shape but no side brighter than the color (each face: its hue x min(its luminance
  / the cube's mean, 1) x the color), so Presidio stays a night track and the lamp pools stay. Without the cap the dark
  ceiling pulled the mean down and the floor side came out 2 to 3 times the color. The color and share come from the
  probes' paint over 20 ft at the look-ahead point (probes out to the light's radius count at 2% weight, so the color
  never drops out between probes), eased over 0.04 s (`Params::paint_radius`, `paint_ease`): a patch fades in over a
  few feet instead of a wheel at a time. On Presidio the lamp pools still reach 0.94 to 0.98 and fall to 0.16 between
  them within a couple of 0.07 s records. On a day track the paint only adds light (`ProbeSet::paint_darkens` false,
  each face's channel at least what the probes give): Marina paints much of its open road a flat gray (0.39 to 0.59),
  which dimmed the whole car, roof included, under open sky and flickered light and dark as it crossed the patches;
  lamp colors in tunnels still show. A night track is told by its sky model (SKYO1, vertex color x texture) averaging
  under 0.25 luminance: the 2049 day skies are 0.35 to 0.62, Presidio's 0.17 and track 4's 0.08; there the paint
  darkens too, as its darkness and lamp pools are the paint. (2049's in-air colors, 0x64 gray on Marina, and the
  floors' median color, 0x96 on both Marina and Presidio, don't tell them apart.) Only with no probe near (in the air) does it use the wheels' color
  (`rush2::track2049::car_light`, which has 2049's blend to the air color): the first version fell back to it
  whenever no paint probe was within 10 ft, 11% of Marina's frames, and the car flickered between the two.
- Each car part draw of a race car (`car_of_node`, the body subtree and the four wheels) is drawn from a per-car copy of
  its list (`rush2::dl::Copier`, 64 KB per car in 0x80F80000-0x81000000) whose G_VTX load per-car copies of the
  vertices. Every draw copies the vertices again (positions too: the 2049 bodies dent) with each color x the cube at
  the vertex's world normal (ambient cube: the faces weighted by the normal's squared components). The world normal is
  the model normal times the node's rotation and its parents' (`node_rotation`). The draw is wrapped with ENV = 0 and
  PRIM white (alpha kept): Rush 2's tunnel darkening and Rush 2049's PRIM color are left to the probes.
- Normals, once per list: every triangle's front normal is added to its three vertices weighted by its angle there,
  turned around when 0x80119848 (mirror mode: the game swaps every list's winding) is set; vertices at the same place
  (a panel's edge, split for its texture) share one normal unless they face nearly opposite ways (a thin fin's two
  sides); merging only under 45 degrees left a hard line along the beltline. Summing all faces at a
  position by area first left a car's tail lit like its roof: the big trunk lid outweighed the small rear face.
- Not relit: draws another hook has already swapped (the Cube Map sheen layers, the ghost car, the wings), and lists
  the copies have no room for.

Popping on Marina (2026-10-14, `car_probes_test --compare` and `--trace` on eight AI cars' recorded laps):

| Bake | Probes | KB | Bake s | Neighbor diff mean / p95 | Top step p95 | Flicker/min |
|---|---|---|---|---|---|---|
| 10 ft, 128 rays, random spots (shipped) | 54,893 | 1,554 | 0.3 | 0.094 / 0.33 | 0.096 | 35 |
| 10 ft, 512 rays | 54,871 | 1,553 | 1.0 | 0.085 / 0.32 | 0.096 | 28 |
| 10 ft, 128 rays, cell-center spots | 58,674 | 1,661 | 0.4 | 0.086 / 0.30 | 0.094 | 32 |
| 5 ft, 128 rays | 215,566 | 6,104 | 1.1 | 0.060 / 0.20 | 0.095 | 32 |
| 5 ft, 512 rays, cell centers, rotated rays | 234,302 | 6,635 | 4.6 | 0.051 / 0.17 | 0.095 | 27 |

- Neither more probes nor more rays changes what the car shows: the 20 ft sample already averages the neighbors, and
  the large changes are real (Marina's covered sections: down-facing ceilings 20 to 40 ft over the road, found with
  `--above X Y Z`). On open road the probes' top stays 0.99 to 1.00.
- The popping is the 2049 painted color. With it the shown top's 50 ms step (p95) goes from 0.039 to 0.098 and its
  flicker from 34 to 47 a minute; 71% of the lap is on painted polygons, mostly flat grays (0.39, 0.49, 0.59: the
  artists' sun shadows), and 60% of the open-sky driving is pulled under 0.7 by them, switching a wheel at a time.
  Easing the paint over time only softens each switch (0.12 s: 40 a minute); the paint now comes from the probes
  instead (above), which blends it over distance.
- Upper body (the cube at the roof normal and 45 degrees up toward the hood, deck and sides; color swings of any
  channel over 0.015 that turn back, per minute, on the AI laps): Marina 129 from the probes and 142 with paint before
  the side-face, soften, density and smoothing changes, 77 and 107 after; NY Two 119 to 89. A wider or slower blend
  (40 ft, 0.2 s) only reaches 71 on Marina: what is left is the light really changing along the road.
- Marina's tunnel (x 2900 to 3300, z -700 to -1100) alternates long stretches of two authored lamp colors from 2049's
  table, warm (0.99 0.85 0.70) and cyan (0.78 0.96 1.00): the car's beige-to-teal there is the table's, as in
  Original; blending the paint over 20 ft rather than 10 changes the swing only from 0.254 to 0.245 (sd of red minus
  blue).
- New York Two's subway tunnel (a shortcut the AI doesn't take; `--dump` lists every probe) has walls whose ROM vertex
  colors are blue and green (NYTWOSUB_*, e.g. 0 84 170, combiner TEXEL0 x SHADE), so the probes there turn blue, then
  green, over about 1,600 ft after warm lamp-lit stretches: the car's beige-to-teal change in that tunnel is the walls'
  own color.

Test aid: `RUSH2_PROBE_TEST=1` logs car 0's probe faces every second; `=path` logs every car's position and painted
color 20 times a second for `car_probes_test --path RUN_LOG TRACK` (`--trace CAR` prints one car's light along its
path, `--trace 99` where cars find no probe in reach; TRACK `k49:K` is Rush 2049 track K); `RUSH2_PROBE_TEST=cube` also lights every car with
a fixed cube (above white, +x green, the rest dark gray): the roof lit, the tail and underside dark, the +x side (the
left, the game's x points left) green. Checked 2026-10-14: the cube on a Rush 2 car (Seattle) and a Rush 2049 car
(NY Two), Presidio's tunnel start against Original, and the bake times above in game.

## Soft particles (removed)

A Soft Particles option (blended 3D draws faded by `saturate((surface w - pixel w) / distance)` in RasterPS.hlsl,
with the depth buffer bound for reading) was tried and removed on 2026-10-11. Rush 2's smoke puffs are wide sprites
lying near the road, so they never cut a hard line into it and looked the same either way, while the exhaust flame
sprites, which sit inside the cars' tail pipes, lost their bright cores (fully at a 3 ft fade, partly at 0.75 ft).

## Testing

Configs in a linked test copy (`python tools/link_copy.py tmp/ao` while `build/` is in use) are its own
`graphics.json`; set `ambient_occlusion` there. A race: `RUSH2_EXE=tmp/ao/Rush2Recompiled.exe
RUSH2_ARGS="--autostart" py tools/rush1/shots.py OUT "8:START,11:A,14:A,17:A,20:A,24:A:30" 30 36 42`. At 36 s the
curb and wall bases of the first stretch show the occlusion. Frames
from separate runs differ too much to diff; to see what a pass computes, return its values from
`AmbientOcclusionApplyPS.hlsl` for a test build. Checked with MSAA 4x and off, and in 2 player split screen.
