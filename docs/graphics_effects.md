# Graphics effects: ambient occlusion and car reflections

Renderer effects the original game doesn't have, in Settings > Graphics > Effects and switchable while the game
runs. It is a change to RT64, kept in `lib/patches/rt64.patch` (edit under `lib/rt64`, then
`python tools/lib_patch.py save rt64`).

| Option | Values | Default | RT64 global |
|---|---|---|---|
| Ambient Occlusion (`ambient_occlusion`) | Off, Low, Medium, High = strength 0, 0.35, 0.6, 0.85 | Off | `RT64::AmbientOcclusionStrength` |
| Sheen Style (`car_sheen_style`) | Texture, Reflections | Texture | none: the game marks draws with `gEXSetCarReflection` |

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

Settings > Graphics > Sheen Style = **Reflections**: the cars' paint reflects the scene around them instead of
sliding a sheen texture over it. Hot-swappable (the game decides per draw). Car Sheen still picks which cars have
sheen; under **Original** the 2049 cars have none, so use **All On** to see them reflect too.

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
2. At the view's first marked draw, `submitCarReflection` draws the cube:
   - `CarReflectionCS.hlsl` puts every vertex back in the cube's space from its screen position. It inverts only the
     view projection's x, y and w columns (`CarReflectionCB::invClip`, the car's center folded in), so the depth's
     precision loss doesn't matter and anything the RSP did to the vertex (interpolation, `G_MODIFYVTX`) is taken in.
     It then projects each vertex to the six faces: a 90 degree perspective with an infinite far plane (depth
     `1 - near / w`), written as screen positions the raster vertex shader turns back into clip space with
     `FramebufferParams::resolution`. Each face's near plane is the car's half size on that axis plus 5%, so the
     car's own body is clipped out of its reflection; the wheels still show in the lower faces.
   - `CarReflectionNormalCS.hlsl` gives the marked draws' vertices smooth normals: each vertex sums the area-weighted
     normals of the draw's triangles touching its position. The layers' vertex colors are white with no normals, and
     normals from the depth buffer would make each polygon a flat mirror.
   - The view's own draw calls, with their own pipelines, textures and render params, draw each face of a 768 x 512
     atlas (six 256 px faces, 3 x 2) that has the color target's format and sample count, so those pipelines fit it.
     The atlas is cleared to the view's fill color (the sky or fog color, for what the draws leave empty) and
     resolved when multisampled. Decals in the faces test against the dummy depth buffer, so they may be missing.
   - The faces mirror (`CarReflectionCB::mirror`) when the view projection's x, y, w part, counting the viewport's
     flips, has the other handedness, so triangles keep their winding and back faces stay culled.
3. Each marked draw is drawn by `drawCarReflection` (`CarReflectionVS.hlsl`, `CarReflectionPS.hlsl`) instead of its
   own combiner: the view off the smooth normal into the cube (face by the largest axis, the face layout in
   `CarReflection.hlsli`), blended over the paint with Fresnel alpha `0.12 + 0.88 (1 - cos)^5` x 0.85, depth tested
   `LESS_EQUAL` (nudged 1e-6 towards the camera) against the paint it lies on, no depth write.

Limits: the cube only has what the view drew, so scenery the game itself left out (its PVS by camera region, LOD
distances) is missing; RT64 doesn't run `G_CULLDL`, so the placement box culls don't remove anything; draws in
other projections (a sky drawn with its own projection) are left out; one cube per view. Cost per view with marked
cars: a compute pass over the frame's vertices, the view's draws six more times at 256 px, and the normals.

Not yet checked in game (written in a session without a GPU): look for inverted faces (mirror sign), the car showing
in its own reflection (near planes), and the sky in the up face.

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
