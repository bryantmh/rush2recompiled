# hqx

hq2x and hq4x pixel-art scalers by Maxim Stepin, as packaged by Cameron Zemek (https://github.com/grom358/hqx, commit
a1c7d415f4bb7b947c4c2e47faf220b0148f4bac). LGPL 2.1 or later (COPYING). Used by texture upscaling's HQ2x and HQ4x
modes (src/texture_upscale_images.cpp, docs/texture_upscaling.md).

Changes from upstream:
- `common.h`: `rgb_to_yuv` computes the conversion per pixel instead of reading init.c's 64 MB lookup table (same
  formula), so init.c, hqx.c (the command line program) and hq3x.c are left out.
- `hqx.h`: `HQX_API` is empty, since the code is built into the executable.
