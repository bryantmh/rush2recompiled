# Graphics effects: ambient occlusion

A renderer effect the original game doesn't have, in Settings > Graphics > Effects and switchable while the game
runs. It is a change to RT64, kept in `lib/patches/rt64.patch` (edit under `lib/rt64`, then
`python tools/lib_patch.py save rt64`).

| Option | Values | Default | RT64 global |
|---|---|---|---|
| Ambient Occlusion (`ambient_occlusion`) | Off, Low, Medium, High = strength 0, 0.35, 0.6, 0.85 | Off | `RT64::AmbientOcclusionStrength` |

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
