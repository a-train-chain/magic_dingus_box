# shaderstat - what the Pi 5's shader compiler makes of one Tube program

A developer tool, run ON THE PI as an ordinary user beside the running kiosk. It is
not built by CMake, not installed and not run by CI.

It opens the GPU's render node (`/dev/dri/renderD128`) through GBM + EGL (or EGL's
surfaceless platform when that fails) and makes an OpenGL ES 3.1 context with no
window. It never opens a card node, never becomes DRM master and never touches KMS,
so the kiosk keeps the display. Mesa's on-disk shader cache is switched off for the
process (a cache hit would skip the compile and its statistics, and the cache lives
outside the tool's folder).

## Build (on the Pi)

    g++ -std=c++17 -O2 -Wall -Wextra -o shaderstat shaderstat.cpp -lEGL -lGLESv2 -lgbm

## Inputs

- The assembled sources, exactly as the kiosk compiles them, from the Mac:
  `MDB_WRITE_TUBE_GLSL=<dir> ./build-tube3/test_app_unit "[tube_shaders]"`
  (`<program>.hw.frag` / `<program>.sw.frag` and `fullscreen.vert`).
- Uniform files, from the Mac:
  `MDB_WRITE_TUBE_UNIFORMS=<dir> ./build-tube3/test_app_unit "[tube_shaders]"`
  writes, per look at a 1440x1080 Modern TV picture, `<look>.txt` (mid-run: warm-up
  over, phases non-zero, the limiter fed), `<look>.degauss.txt` (a degauss under way)
  and `<look>.synthetic.txt` (NOT a driver frame: power-off values and the character
  blocks no look uses, so a comparison reaches every branch). Format: `name=v[,v...]`
  per line, `#` comments, and three directives: `@fb=W,H` (the framebuffer and scene
  texture), `@pic=X,Y,W,H` (the picture in it, top-down) and `@mask=W,H,<hex RGB>`
  (the look's mask tile).

## What it reproduces

Mesa's V3D shader-variant key includes the sampler formats and the render-target
format, so the tool binds a texture of the renderer's internal format on the
renderer's unit for every sampler the program declares (`ui` SRGB8_ALPHA8 sampled with
DECODE when the source has `SCENE_HW_DECODE`, else RGBA8; `glowTex`, `prev`, `quarter`
SRGB8_ALPHA8; `signalTex`, `linesTex` RGBA16F; `glassTex`, `maskTex` RGBA8, the mask
NEAREST + REPEAT; units as `kSamplerUnits` in `tube_renderer_internal.h`) and draws
into the program's own target as `tube_renderer_passes.cpp` does: an `OUT_HISTORY`
program into a picture-sized SRGB8_ALPHA8 target with FRAMEBUFFER_SRGB on and
`uOrigin` 0,0; any other into a framebuffer-sized RGBA8 target, cleared, at the
picture's viewport with `uOrigin` its corner. (The kiosk's screen is XRGB8888; the
tool's RGBA8 reproduced the kiosk's statistics exactly on 2026-11-15.) The texture
contents are procedural (gradients, a one-pixel checker, a ripple, steps), the same
for every run, so a comparison sees a wrong tap or a missing term. `uMaskSize`,
`uSceneMap` and `uSceneClamp` are computed from the directives as the renderer
computes them; a value in the file wins, except `uOrigin`, which is the target's.

## Usage

Always under a timeout (a compile that hangs the driver is a finding, not something
to wait for):

    timeout 20 env V3D_DEBUG=shaderdb ./shaderstat [--uniforms FILE] VERT FRAG
    timeout 20 env V3D_DEBUG=shaderdb ./shaderstat --uniforms FILE --time N VERT FRAG
    timeout 20 env V3D_DEBUG=shaderdb ./shaderstat --uniforms F1 [--uniforms F2 ...] --compare VERT A.frag B.frag

Prints, per program, the fragment shader's statistics from Mesa's shaderdb line:

    out_rich_raster_ag.hw.frag: 474 inst, 2 threads, 47 max-temps, 0:0 spills:fills | target SRGB8_ALPHA8 ...

- **Statistics** are the primary evidence. V3D runs a fragment shader on 4 QPU threads
  only when its live temporaries fit the smaller per-thread register file; at 2 threads
  the same pixels take roughly twice the shader time. Without `V3D_DEBUG=shaderdb`
  there is no line. A second program with byte-identical source compiles nothing new
  (Mesa reuses the variant), so it prints no line either.
- `--time N` draws N frames, each bracketed by `glFinish`, and prints the median, mean
  and minimum wall-clock ms. The production kiosk shares the GPU and the timing
  includes the clear and the submit: treat it as RELATIVE and noisy (the minimum is
  the most repeatable figure), never as the kiosk's GPU time.
- `--compare` renders both fragment programs with identical inputs, once per uniform
  file (one compile each), reads both back and prints the largest per-channel
  difference (in 1/255 steps) and how many pixels differ by more than 1/255. Exit 3
  when any pixel does. `--float` renders into RGBA32F instead (a finer comparison; a
  different render-target format, so not the kiosk's variant and its statistics are
  not the kiosk's).

Exit codes: 0 fine; 1 a GL/EGL failure, a compile or link error (the log is printed)
or an incomplete framebuffer; 2 usage, an unreadable source or uniform file, or a bad
line in one; 3 a comparison found a difference beyond 1/255.
