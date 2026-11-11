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
| Sky Fog (`sky_fog`) | Off, On | Off | none: the game marks each race view with `gEXSetSkyFog` ([Sky fog](#sky-fog)) |
| Car Lights (`car_lights`, not in the menu: TODO.txt) | Off, On | Off | none: the game places light markers with `gEXSetCarLight` ([Car lights](#car-lights)) |
| Fill Scenery Gaps (`scenery_fill`) | Off, On | Off (Enhanced setup: On) | none: `gEXSetTwoSidedWalls` around the track's models, and the game draws the ground underlay ([Fill scenery gaps](#fill-scenery-gaps)) |

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
  turned around when 0x800D0190 (mirror mode: the game swaps every list's winding) is set; vertices at the same place
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

## Car lights

**Not in the menu and never on** (src/config.cpp registers no option; TODO.txt, General) until the lens glow below
works. Car Lights = **On** was: on a night track, a braking car's brake lights cast red light on the road and
whatever else is close behind it (src/car_lights.cpp, lib/rt64 `CarLightPS.hlsl`). It replaces a first try that drew
the PlayStation SF Rush's flare sprites at guessed points, which didn't sit on the cars' real lights.

- **Night tracks:** `rush2::track2049::night_race`, a converted course (Rush 2049, custom, arcade) whose fog color is
  near black (luminance under 0.1). Rush 2049's fog table (main 0x80114658, 3 bytes per track) is gray-blue for the
  day tracks (0.55-0.61) and black for race tracks 4 and 6 (Presidio) and the stunt and battle arenas; the SF Rush
  PlayStation night versions set #080808. Stock Rush 2 courses never take it.
- **When:** while Rush 2 lights the car's tail lights itself (braking, the car's flags 0x1000 set and 0x10 clear:
  func_800599A0, read in its hook `rush2_calspeed_brake_lights`), so the light and the lenses come on together. A
  side goes out once its rear corner's panel (panel 2 rear right, 3 rear left: car state +0xE4, masks 0x800CE188,
  shifts 0x800CE180) is damaged at all: level 1, the most collisions give (func_8006B730, func_8009A264 and
  func_800715E8 stop at `slti 2`), the level that swaps in its damaged model. A wrecked car, a car out of the race
  and a ghost show none.
- **Where:** the middle of what the game lights on that side, found from the car's own drawing
  (`Finder`, one car a frame, again when its type, parts or damage stage change): its body lists (frame, corners,
  top: `rush2::car_types::part_record`) walked with their texture state (G_SETTIMG, G_SETTILE, G_LOADBLOCK /
  G_LOADTILE, G_SETTILESIZE, G_TEXTURE), and each lit texel put back on its triangle by the triangle's texture
  coordinates (barycentric), averaged per side of the body (x sign). Lit texels: Rush 2's palette lighting's CI8
  entries 161-175 (Rush 2's cars, Rush 2049's on the N64 ROM); for a California Speed or Dreamcast car the texels its
  lit copies change (`rush2::car2049::brake_light_sources`). A mod car's or livery's brake light lists
  (docs/car_mods.md#brake-lights) are its lights instead, each triangle by its area. The light sits 0.3 ft behind that
  point. Cars with no brake lights in the game (the SF Rush PlayStation cars) cast none.
- **Only backward:** it is a tail light, not a glow round the car. The light shines in a cone straight back along the
  car (the marker's world transform's third row, negated: `CarLightCB::direction`), full on that axis and fading to
  nothing 70 degrees off it, so the road and a car close behind take it and the car it is on (its body, roof and
  bumper, all ahead of or beside the lens) doesn't. A light shining every way lit its own car's whole body.
- **Drawing:** each lit side is a marker placed through `rush2::battle_render` (slots 186-201, model `CAR_LIGHT` at
  0x80E1F200: a sliver of a triangle between `gEXSetCarLight(31, 2, 1, 48, 192)`, red at 3x strength over 12 ft, and
  its end), so it rides with the car through frame interpolation. The pass multiplies what is drawn, so the strength is
  high for the dark night roads: 1.5x over 10 ft left them unchanged. RT64 doesn't draw a marker (`InstanceDrawCall::triangles.carLightIndex`); it takes
  the origin of the marker's interpolated world transform (`lerpWorldTransforms`) as a light, relative to the camera
  through the same reconstruction Sky Fog uses (`viewReconOf`), and once the view's 3D draws end (its next 2D draw or
  the end of the scene, after ambient occlusion) runs `CarLightPS` over the screen box of the cube round the light's
  reach: each pixel's position from the depth buffer, light falling off as (1 - d / 12 ft)^2, times the cone, times
  0.35-1 by how squarely the surface faces the light, and the pipeline multiplies it in and adds it (dst * (1 + light), blend
  DEST_COLOR / ONE), so a surface brightens by its own color: the road turns red, black stays black. The pipelines are
  made with the shader library at startup, so the first light doesn't hitch.
- **Glowing lenses (broken, the reason the option is out):** in game the parts drawn after the overlay (the left
  rear corner, the roof) came out black, with the parts' texture commands replayed after it or not.
  `RUSH2_CAR_LIGHTS_DIAG_SKIP=1` leaves the overlay out (the car is right then); `RUSH2_CAR_LIGHTS_DIAG_NOTEX=1`
  draws it without its mask load, the next thing to try. The design: on a converted track every car part is multiplied by the track's car light color
  (`rush2::car2049::light_model`, dark on a night track), so Rush 2's lit tail light texels (0xF801) came out a dull
  dark red. While a side's light shines, each body part holding lit texels (the finder keeps their triangles: part,
  side, vertices, texture coordinates state) is followed in the model draw hook (`rush2::car_lights::model_draw`, after
  light_model in src/widescreen.cpp, so it has the part's own matrix) by those triangles again: an IA8 mask of the lit
  texels laid over the part's own tile (size, wrap and G_TEXTURE scale copied), PRIM = Rush 2's lit red, no shade or
  fog, blended as a decal (G_RM_ZB_XLU_DECAL2) inside a push and pop of the other mode, combiner and PRIM. Masks and
  lists sit above 16 MB (`recomp::alloc`, read through `gEXSetRDRAMExtended`), two 32 KB buffers a car written in turn.
  A mod car's or livery's brake light lists need none: they already draw unshaded (`brake_render_mode`).
- **Testing:** `RUSH2_CAR_LIGHTS_LOG=1` prints each car's light points as they are found and each light turning on or
  off; `RUSH2_CAR_LIGHTS_TEST=1` lights every car as if braking. For example `tools/test_race.py chinatown_night
  --cars 3 --mod-dir <PS1 mod> --config graphics.car_lights=On --env RUSH2_CAR_LIGHTS_TEST=1 --shots 24,28,31,34,38`
  (shot times from launch).

## Sky fog

Settings > Graphics > Sky Fog = **On**: the race's sky is hazed toward the fog color by height above the horizon, as
real haze does, so the fogged skyline blends into the sky instead of standing out as a pale band against it. The game
fogs the world by distance (`gSPFogPosition` from the view's +0x4C / +0x4E in `view_fog_setup_8007C624`) but never the
sky: Rush 2's cloud dome (`sky_setup_800A45A8`, only its rim faded by vertex alpha), the sky models (Las Vegas
SKYO1, the stunt arena, Rush 2049 tracks, custom `SKY` meshes) and the clear color. Hot-swappable: decided per view
each frame.

**Model.** An exponential height fog around the camera: density sigma at the camera's height falling by e every H above
it. A ray at elevation e passes through optical depth sigma H / sin e on its way out, so the sky keeps
`exp(-haze / sin e)` of its color, haze = sigma H. At the horizon that is none of it, the fog color, which is what the
world's fog reaches at the far plane, so the skyline meets the sky in the same color; overhead the sky stays nearly as
it was. The world's own fog is left as the game draws it.

**Game side** (`src/sky_fog.cpp`): `rush2_draw_distance_projection` (before guPerspective at 0x8007CB5C) passes each
view's near, far and fog range to `rush2::sky_fog::view_setup`, and `rush2_interp_projection` emits
`gEXSetSkyFog(haze in thousandths)` with the view's projection, 0 when off or outside a race (game states 3, 6, 9, 10:
the car and track selects' 3D draws over 2D backgrounds, which would be hazed too).
- The game's fog at distance d is (D - min) / (max - min) with D = 500 (z / w + 1) and z / w = a - b / d
  (guPerspective's a = (f + n) / (f - n), b = 2fn / (f - n)): linear in 1 / d from its start to its end. The draw
  distance factor puts both ends k times farther (src/draw_distance.cpp).
- sigma = ln 2 / the distance where the game's fog is half (1 / d halfway between the ends): a uniform medium's fog
  1 - exp(-sigma d) then agrees with the game's at its middle and its extremes.
- H = 200 ft, well under the dome's 900 ft cloud lid: a shallow layer under the clouds. One player with FOG 2 (fog
  from about 260 ft to 1500 ft, draw distance Original) gives haze 0.31: the zenith keeps 73% of its color, 10 degrees
  up 17%. Thicker FOG options and fog zones raise the haze with the
  world's fog.
- **FOG 1 on a converted course with a sky model is no fog** (`rush2::sky_fog::fog_removed`), whatever Sky Fog is set
  to. Rush 2049's own lowest fog level is no fog (its level 0 leaves every race view at fog range 1000 / 999,
  [Rush 2049 race research](rush2049_research/race.md#fog)), and the arcade and custom courses with a sky model follow
  it. A course counts when a converted one is applied in the slot raced (`rush2::track2049::converted_race`: Rush 2049
  tracks and arenas, arcade and custom tracks; the 2049 arenas sit in the stunt arena's slot, so the slot alone doesn't
  tell) and its view has a sky model (a transform in the moved 0x80125B68; views 2 and 3 draw views 0 and 1's).
  Stock Rush 2 courses keep Rush 2's fog at FOG 1 (from about 477 ft), Las Vegas and the stunt arena included, and so
  do courses with the cloud dome (SF Rush, California Speed, custom tracks with `sky = "clouds"`).
  `rush2_draw_distance_projection` replaces the view's fog with factors 0 (`gEXSetFogFloat`), so the game's own fog
  values, which the fog zones read, are untouched; the view's haze is 0. Sky Fog itself only adds the haze.
- **Sky draws** are marked with `gEXSetSkyDraw(1)` ... `(0)`: the dome's pieces in `rush2_interp_poly_end`
  (src/interpolation.cpp, which redraws them), and the sky model, whose scene node's transform (node +4) is one of the
  per-view sky transforms, in `rush2_sky_fog_model_draw` (from `rush2_model_draw` at 0x8007AF84, $s7 = node, $fp =
  list, wrapped in a side list). Only marked while Sky Fog is on.

**RT64 side** (`lib/patches/rt64.patch`): `G_EX_SETSKYFOG_V1` (0x36) sets `DrawExtendedFlags::skyFog`. When the frame's
draws are sorted (`FramebufferRenderer::addFramebuffer`), each perspective view with marked draws gets one
`SkyFogPass` (projections with the same viewport in a framebuffer pair are one view): the inverse of its view
projection's x, y and w columns (what the GPU drew with, interpolated frames included) with the viewport folded in
(RT64's screen y is `-clip.y / w` times the viewport scale, RSPProcessCS.hlsl: a first version missed the flip and
hazed the whole sky as if below the horizon), so
`pixel.x * dirX + pixel.y * dirY + dirW` is the pixel's direction in the world's axes (y up); the depth-to-w constants
of [Depth to distance](#depth-to-distance); and the fog color of its draws that use the standard fog blender cycle
(`Blender::usesStandardFogCycle`). A view with no fogged draw, or no sky draw (`G_EX_SETSKYDRAW_V1` 0x37,
`DrawExtendedFlags::skyDraw`; it shows the clear color, which the game sets to the fog color), gets no pass. The pass
runs once, right after the view's last sky draw, over the area its marked draws covered so far. Not after the view's
last draw: see-through scenery drawn without depth writes (Marina's boat sails) was still at the depth clear there and
came out hazed like the sky (reported 2026-11-05). The 2049 sky models draw first, so the pass hazes the whole view and
the world covers it; Rush 2's dome draws after the world, over the same clear-depth pixels it covers anyway. `SkyFogPS.hlsl` (and
`SkyFogPSMS` for a multisampled depth buffer, sample 0) keeps only the pixels still at the clear (depth 0.99999 and up,
or a w not above 0: the dome's prim depth 0x7FFE included) and returns the fog color with alpha
`1 - exp(-haze / sin e)`, which the pipeline blends over the color target's RGB.

Limits: blended draws without depth writes drawn before a Rush 2 dome (under its rim) are hazed as sky; the haze
follows the camera's height, not the world's (the world's fog doesn't either).

## Fill scenery gaps

Settings > Graphics > Fill Scenery Gaps = **On** (Off is the original game; the Enhanced setup turns it on,
`rules()` in src/setup.cpp): the tracks were built for a 4:3 view from the road, so a wide screen, a high camera or
the wings see past where the scenery ends, sky under the edges of the ground and through open-backed buildings. Two
parts, both decided per frame (hot-swappable). Code: `src/scenery_fill.cpp` (game), `src/scenery_fill_build.cpp`
(the underlay, nothing of the game), `include/scenery_fill.h`.

**Which draws are the track.** `race_load_track_800A5110` calls `placement_walk_80081790` at 0x800A5704, which makes a
scene node per placement record; the scene node count (0x800FAE58) before the call and after it returns (hooks at
0x800A5704 and 0x800A570C, `rush2_scenery_fill_walk_begin` / `_end`) bounds the track's nodes: sections, props,
signs. The sky model (`sky_setup_800A45A8`) and the cars are made elsewhere. `rush2::scenery_fill::model_draw` (from
`rush2_model_draw`, model_draw_8007AA48 at 0x8007AF84, $s7 = node) acts on those nodes only, in a race (game states
3, 6, 9, 10).

**Two-sided walls** (RT64, `lib/patches/rt64.patch`): each track node's model list is called between
`gEXSetTwoSidedWalls(1)` and `(0)` (`G_EX_SETTWOSIDEDWALLS_V1` 0x38). In `RSP::drawIndexedTri`, a triangle drawn with
culling on whose face is within about 17 degrees of vertical (|n.y| < 0.3 |n|, from its vertices through the world
transform: the game keeps the view's rotation in the projection matrix, so the modelview's axes are the world's, y up)
gets its indices pushed again in reverse order: whichever side faces the camera survives the culling, with the front's
texture and vertex colors. Exact per triangle, no shader change (the render flag word is full). **A face that already
has a back side is left alone.** Every other model in a race is called between `gEXSetTwoSidedWalls(2)` and `(0)`:
its triangles are seen but get no copies. The frame's near-vertical triangles in marked draws are kept by plane
(facing yaw in 128 steps, plane distance in steps of 8 units) with their corners, and a triangle gets no copy, and
copies already made there are made degenerate (so the triangle count stays the same every frame, which frame
interpolation matches draws by), when its plane has over it (overlap in the plane, by separating axes):
- a modeled back: a triangle facing the other way within 16 units (1 ft), culled or not;
- a two-sided surface: a triangle drawn without culling, either facing, within 16 units;
- a layered face: a culled triangle facing the same way within 2 units, overlapping by more than half a unit (an arrow
  over a sign board): their copies would fight each other from behind.
History (2026-11-08): copies fought or beat real backs three times. Hawaii's start banner (its back isn't split into
the same triangles as its front, so matching exact corners missed it), and Civic Dreamcast's blinking arrow signs
(z-fighting with the plane match limited to culled backs 1/8 ft away); the user's rule: zero unintended z-fighting.

**One-way road blocks** (issue 62, 2026-11-10): some barricades are one-sided on purpose, so they hide from behind.
New York Downtown's N.Y.P.D. barricade (texture `NYONEBPARK1.11`, drawn in ~45 of its section models) closes the road
back onto the Brooklyn Bridge (the `NYONECHALL7` one, game (-1571, 104, -352)) and side streets; from the bridge's
offramp the original shows the road through it, and a two-sided copy put a mirrored barricade across the road. Such
textures never get copies:
- `one_sided_hashes` (include/scenery_fill.h) lists their RT64 replacement hashes (as Dump Textures' hashes.txt has
  them: NYONEBPARK1.11 = `2E93790202DA2D0A`). The underlay build (`rush2::scenery_fill::build`) finds them in the
  track's container and keeps their texels' file offsets, mipmap levels included (`Mesh::one_sided`, cached with the
  underlay).
- `model_draw`, before each view's first track model, sends the RDRAM address of each (the geometry's slot record
  data pointer + offset) with `gEXSetTwoSidedSkip(address)` (`G_EX_SETTWOSIDEDSKIP_V1` 0x3A; RT64 empties the list at
  each display list's start), once the first 8 bytes there are the file's (the worker can run before the game has
  loaded the track).
- RT64 keeps the texture image address of the last `G_LOADBLOCK` / `G_LOADTILE` (`RDP::extended.lastTexelLoad`;
  palette loads don't count) and makes no copy of a triangle drawn while it is a listed address.
Test: `python tools/test_race.py 1 --cars 0 --drones 1 --user-settings --seconds 60 --shots 45,55 --script "1:B:59"
--env RUSH2_CS_PLACE=-1470,112,-255,-0.71,-0.70,0,0.5` stops the car on the offramp facing the gate (the log says
`[Fill] Two-sided walls leave out 1 of 1 one-sided textures`); add `--exe .release/build/Rush2Recompiled.exe` for a
build without it. `tools/rdram_find.py PID --file ASSET --offset OFS` finds where the game put an asset's bytes.

**Extended ground.** A ground underlay built from the raced track's own models (the same inputs as the
[car light probes](#car-light-probes): the slot's geometry and placement assets as loaded, `rush2::probes::raced_track`,
and the shared containers 0x12 and 0x14, through `rush2::catalog::for_each_placed_triangle`):
1. A 32 ft grid over the ground and 4000 ft past it. A cell is covered where an upward-facing triangle (normal y over
   0.5) lies over its center; it takes the lowest such surface's height, vertex color and texture.
   **Roofs with no floor under them** are not ground: a group of covered cells (neighbors joined where their heights
   are within 24 ft, a dome's curve) of at most 100 cells that stands over 20 ft above all the covered ground within 4
   cells (128 ft) of it, water textures left out, stops counting as covered, so the ground round it fills in under it
   (the floors, step 5, keep it under the roof). A group with no ground near (an island, a pier over a sea plane) stays
   ground. Reported 2026-11-10: the PlayStation Golden Gate's rotunda (Palace of Fine Arts) has no floor under its dome,
   so the dome was the lowest surface in its cells and the underlay rose to it as a green cone, in every time of day.
   On the stock tracks the rule takes building roofs and towers (the Space Needle's top) and fills sky gaps beside
   them (LA); checked against the build without it with `car_probes_test --fill` on every Rush 2, SF Rush and PS1 track.
2. Heights and colors of the rest by pull-push (a pyramid of averages, then each empty cell blended from the level
   above): the ground carries on smoothly from its edges and flattens far away.
3. Textures: the ground textures covering the most cells (up to 8, each at least 2% of the ground), each one's coverage
   pull-pushed the same way; each cell takes the strongest. Big areas (grass, water, sand) carry on. Taking the
   nearest covered cell's texture instead drew roads that reach the edge on out as stripes.
4. Drawn: the fine layer, the empty cells within 10 cells (320 ft) of the ground and the covered cells beside them,
   out to whole 256 ft cells; the coarse layer, the 256 ft cells with no ground or fine cells in them, out to the
   margin. The two never overlap: a coarse quad under fine ones had to come down to the lowest of them, on a shore the
   sea's level, which stood the land over it on a wall. Where the fine layer ends over a lower coarse surface, each
   outer fine edge hangs a **skirt** down to it (the coarse height there less 1 ft), so the sky never shows through the
   step (steep slopes, seen in game on Golden Gate snow).
5. **Never through the track**: every triangle drawn, whichever way it faces, sets a floor on each cell it overlaps (a
   lower bound of its height there: the larger of its plane's lowest at the cell's corners and its lowest vertex). A
   fine corner sits 2 ft under the heights and floors of its four cells, a coarse corner 6 ft under the fine corners
   within a coarse cell of it that touch the track (its ground or any of its triangles), so every quad (a mix of its
   corners) stays under everything over its cell; far from the track a coarse corner follows its own height. Heights
   from upward faces alone let the sheet come up through embankments and walls falling away from a road's edge
   (Chinatown, reported 2026-11-08).
6. **The sea**: a custom track's `SEA` mesh ([custom tracks](custom_tracks.md#sea): model FILLSEA, never placed) and a
   [diorama's](#diorama-guides) water. A cell over the sea is sea, at its height and with its texture, unless the
   track's own ground there is within 30 ft of it (a quay, a ship's deck); higher ground is a deck over it (a bridge),
   with the sea drawn under. The sea is left out of the texture vote and the color fill (it turned the inland city to
   water), and only the track's own ground counts as covered, so open sea is drawn coarse.

**Pages of pictures.** A texture of which a triangle maps only a rectangle (the corners' span in texels, whole repeats
taken out, under 3/4 of the texture on an axis) is a source of its own: that rectangle, resampled (nearest) to 16-64
texels a side. The PlayStation SF Rush's textures are pages of many pictures (the converter's 32 x 32 tiles of its 256 x
256 pages, the pictures at any offset), each face a quad mapping one; the whole page repeated drew a patchwork of
facades and fences over the snow. Only the textures the strips draw are kept.

### Diorama guides

Every Rush 2 and SF Rush track's track select diorama is a small model of the course and what lies round it: the
red route line, the sea, parks, city, beach, hills. `src/scenery_fill_guide.cpp` lays it over the track and the
builder follows it past the track's own ground (reported 2026-11-10: Golden Gate's ground carried on as a dome with a
bump where the bay is, Embarcadero's city texture ran on under the ships).

- **The fit** (`fits` in src/scenery_fill_guide.cpp, made with `tools/diorama_fit.py`): a similarity transform (scale,
  turn, mirror, offset) that lays the diorama's red route line (triangles whose color is mostly red) over the track's
  AI path, seen from above: a search over turns and mirroring, then ICP both ways. Mean distances 36-300 ft against
  courses 4,000-13,000 ft long. Rush 2's eight race tracks (NY One is NYLTRACK, NY Two NYUTRACK), SF Rush's seven
  (TRK1O2 ... TRK7O5, from its asset 10 through `rush2::track1::convert_container`), their arcade versions (Rush The
  Rock: the N64 frames, within a few feet; arcade Sunset's shorter figure 8 fit badly on its own, so it takes the N64
  fit too) and the PlayStation versions of tracks 1-3 (the N64 courses turned a quarter, fitted on their own; the
  snow and night ones carry the PlayStation's water colors). The stunt and battle arenas' dioramas have no route line.
- **Kinds** by the diorama's color (its texture's mean times its vertex colors): route (red), water (blue), green,
  sand (tan, brown), city (the rest: greys, whites). Per 32 ft cell: the lowest land surface (a building stands on the
  ground) gives the kind and the lie; water where only water is there. Past the diorama's edge both carry on from it
  by pull-push (the sea round an island stays sea).
- **Sea**: the diorama's water where the track has no ground, at the track's own water (its blue textures under the
  diorama's water: the commonest one, at its median height), else the SEA mesh's, else a flat texture of the version's
  water color (FILLWATER, with a faint ripple) 4 ft under the lowest ground within 320 ft of the water.
- **Lie**: the diorama's land height times a factor fitted (least squares, 0 to 1) to the track's ground where both
  are (the dioramas' hills are drawn taller: 0.2-0.7 on hilly courses, 0 on the flat city ones, 1 on The Rock and
  Alcatraz), plus the track's ground's difference from that, filled in from where it is known (land only: the sea's
  level pulled the land down to it a few hundred feet out). Land within 12 cells (384 ft) of the sea slopes down to the
  sea's level (smoothstep), a shore instead of a cliff.
- **Textures**: each kind of land takes the track's commonest texture in the diorama's areas of that kind, weighted by
  how near its hue (color over brightness, so snow and night versions compare alike) is to the diorama's there
  (1 / (1 + (d / 0.1)^2)), never a water texture (Hawaii's sea plane is the lowest surface under its land) or a
  striped one (texels changing over twice as much across one axis as along the other: lane lines measure 2.5-7, ground
  1.0-1.2; a city's streets repeated over the city drew lane lines across it). A kind with under 20 cells of the
  track's ground keeps the plain vote. The snow and night versions use their own pages, so their ground is snow or
  dark.
- **Game side**: `guide_key` (view_begin): an SF Rush track (`rush2::track1::race_track`) is `sfrush:<n>`, a custom
  track its folder (`rush2::track_mod::race_folder`), a stock Rush 2 slot `rush2:<SLOT>`; the worker reads the
  diorama containers once (Rush 2's asset 3, SF Rush's asset 10) and the guide's hash joins the cache key.
- **Offline**: `car_probes_test --fill --guide 2 r1:1 dir:<converted>=<key>` (r1:N converts SF Rush track N from
  RUSH1_ROM) also writes `tmp/scenery_fill/<TRACK>_guide.glb`, the diorama as laid in its kinds' colors.

The textures are the track's own, re-encoded in their own format and palette (`encode_texture`, checked by decoding
them as RT64 does, `image_of`, against the catalog's image), so texture packs replace them by content
([derived replacements](texture_upscaling.md)); textures that aren't a power of two or don't fit one TMEM load are
left out. Texel size in the world comes from the texture's upward triangles (world area over UV area); the coarse
layer doubles it until a cell fits a vertex's s10.5 range. Output: chunks (16 x 16 fine or 4 x 4 coarse cells) of
strips (rows of up to 15 quads, 32 vertices). The stock tracks build in about 0.02 s to 0.5 to 2.5 MB, 10,000 to
40,000 quads.

**Game side.** `view_begin` (from `rush2_interp_view_begin`) starts a worker when the raced track changes: it loads
the underlay from the track cache (`scenery_fill_<hash>` blob, keyed on the inputs' bytes and `build_version`) or
builds and saves it, then writes it into one of two 4 MB arenas from `recomp::alloc` (above 16 MB, used in turn so a new
track never overwrites the one drawn): per texture a load list (TEXTLUT, LOADTLUT, LOADBLOCK, render tile), per chunk
its vertices (s16 in 1/16 ft from the chunk's center) and a list of G_VTX / G_TRI2. In each view, before the first
track model drawn (after the sky models, which draw first without depth; before the rest of the world), `model_draw`
calls the view's list between `gEXSetRDRAMExtended(1)` and `(0)`: the chunks in front of the camera and within the
view's far plane (view +0x38 times the draw distance factor), each in a matrix group of its own
(0x7vgggccc: view, view generation, chunk) under a float matrix loaded as the whole modelview (translate (center -
camera) x 16, times the view's root modelview), as `src/rush2049/battle_render.cpp` does. Its render state sits
between `gEXPush/PopGeometryMode`, `OtherMode` and `CombineMode`: 2-cycle, TEXEL0 x SHADE, `G_RM_FOG_SHADE_A` +
`G_RM_AA_ZB_OPA_SURF2`, fog and depth like the track, no culling (a mirrored race needs none); the palette cache
(0x80111954) is cleared after, as the battle models do.

Offline: `tools/car_probes_test.cpp --fill [--guide] [TRACK ...]` (build with `tools\car_probes_test.bat`) prints each
track's underlay size and writes `tmp/scenery_fill/<TRACK>.glb`, the track with the underlay as node SCENERYFILL, for
`tools/glb_view.py`. TRACK: a Rush 2 slot, `r1:N`, or `dir:<folder>` (tools/track_mod_test output).

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
