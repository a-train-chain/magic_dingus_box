# Zero-copy video (EXPERIMENTAL, Pi 4B only)

Status: **unvalidated on hardware.** It lives on the
`experimental/zero-copy-dmabuf` branch and must not be merged until the
checklist below has passed on a real Pi 4B. It is OFF by default. With the
flag unset, the kiosk does exactly what it did before. The only difference
is one extra startup line: `Video upload: copy (MDB_VIDEO_ZERO_COPY not set)`.

## What it changes

Today every decoded frame goes through the **copy path**:
`gst_video_frame_map`, then `glTexSubImage2D` for each plane. At 1080p24-30
that is roughly 75-95 MB/s of CPU and Mesa work on the render thread.

On a Pi 4B, `v4l2h264dec` (hardware H.264) can hand out DMABuf-backed
buffers. With the experiment on, the **zero-copy path** does this instead:

1. The appsink lists `video/x-raw(memory:DMABuf)` caps ahead of the old
   system-memory caps. It accepts legacy `format={NV12,I420}`, and also
   GStreamer ≥ 1.24 `format=DMA_DRM, drm-format={NV12,YU12}` (linear only).
   The old I420/NV12/RGBA system-memory caps stay as the fallback, which
   software decoders such as `avdec_h265` still use.
2. Each dmabuf frame is wrapped in an `EGLImage` through
   `EGL_EXT_image_dma_buf_import`, using the fourcc, plane fds, offsets and
   pitches from the buffer's `GstVideoMeta`. The colour-space and range
   hints come from the stream's colorimetry. A modifier is passed only when
   `EGL_EXT_image_dma_buf_import_modifiers` exists.
3. The EGLImage is bound to a `GL_TEXTURE_EXTERNAL_OES` texture. A
   `samplerExternalOES` shader draws it with the same quad, viewport,
   letterbox/inset math, bezel and CRT post-processing as before. The
   driver converts YUV to RGB.
4. Each GstSample stays held until the frame after next has been imported
   (a one-frame-lag ring of 2). This stops the decoder from recycling a
   buffer while the GPU may still be reading it. Up to 12 EGLImages are
   cached per dmabuf inode, so a recycled pool buffer reuses its image.
   Everything is released when the pipeline stops (load, advance, game
   launch, shutdown) and when the kiosk returns from RetroArch.
5. **Fallback.** A frame that is not a dmabuf (software decode, or the v4l2
   pool copying under pressure) goes through the copy path for that frame
   only, and is not counted as an error. An import error also falls back
   for that frame, with one WARN. **30 consecutive** import errors turn the
   zero-copy path off until the kiosk restarts.

All three gates must pass, or the copy path stays:

| Gate | Where |
|---|---|
| `MDB_VIDEO_ZERO_COPY=1` (exactly `1`) | environment |
| board profile `video_dmabuf_import_candidate` (Pi 4B: true; Pi 5 / Unknown: false) | `platform/platform_profile.cpp` |
| `EGL_KHR_image_base` + `EGL_EXT_image_dma_buf_import` + `GL_OES_EGL_image_external(_essl3)` + resolvable entry points | probed at startup only when the first two pass |

Code: `src/video/zero_copy_policy.h` holds the pure logic and is unit-tested
in `tests/video/test_zero_copy_policy.cpp`. `src/video/dmabuf_importer.*`
holds the EGL/GL/GStreamer code. The wiring is in `gst_player.cpp` (caps,
stop hook), `gst_renderer.cpp` (per-frame branch, draw) and `main.cpp`
(decision).

## Enable on a Pi 4B

The binary must be built from this branch:
`PI_HOST=magic@<host> magic_dingus_box_cpp/dev/pisim/pisim.sh push`.

```bash
sudo systemctl edit magic-dingus-box-cpp
# add:
#   [Service]
#   Environment=MDB_VIDEO_ZERO_COPY=1
sudo systemctl restart magic-dingus-box-cpp
```

This writes `/etc/systemd/system/magic-dingus-box-cpp.service.d/override.conf`.
On a Pi 5 the flag is ignored with a WARN (`board profile does not allow
dmabuf import`), which is correct behaviour.

## What to look for

### 1. Journal: which path is active

```bash
journalctl -u magic-dingus-box-cpp -b | grep -E "Video upload|Zero-copy|Appsink caps"
```

| Line | Meaning |
|---|---|
| `Video upload: zero-copy dmabuf requested (EXPERIMENTAL; sampler ESSL3 external, modifiers yes)` | All gates passed at startup |
| `Appsink caps: DMABuf preferred ...` | DMABuf caps are on the appsink |
| `Video upload: zero-copy dmabuf (NV12)` (or `(I420)`) | **The goal.** The first frame of this stream was imported |
| `Video upload: copy (decoder delivered system memory)` | The decoder did not negotiate DMABuf. Expected for HEVC (`avdec_h265`) and any software decode. For **H.264** it means the experiment is not reaching the decoder (see the troubleshooting table) |
| `Zero-copy: dmabuf import failed (<reason>)` | One WARN with the first failure's reason |
| `Zero-copy: 30 consecutive import failures ... disabled until the kiosk restarts` | Permanent fallback |
| `Video upload stats (previous stream): N zero-copy, M copied, K import failures` | Per-stream counts, logged when the next stream starts. For H.264, M and K should be 0 or close to it |

### 2. CPU, before and after

Play the same 1080p H.264 file for 60 s with the flag off, then on.
Use the main-UI playlist and a Media Browser movie, because they use
different viewports.

```bash
top -d 5 -p "$(pidof magic_dingus_box_cpp)"   # note %CPU after ~20 s settles
# or, per thread:
top -H -d 5 -p "$(pidof magic_dingus_box_cpp)"
```

Expect the kiosk's %CPU to drop noticeably. The render thread no longer
maps or uploads ~3 MB per frame. Also note `vcgencmd measure_temp`, and run
`vcgencmd get_throttled` to confirm the board was not throttled during either
run. Record both numbers in the PR before merging.

### 3. Picture correctness

Check each item with the copy path and the zero-copy path:

- **Geometry.** There should be no diagonal shear (wrong pitch), no green
  or garbage band at the bottom or right edge (wrong offsets or height
  padding), and no mirroring or flipping. Check 1080p, 720p and an odd
  width such as 854x480.
- **Chroma.** There should be no green/magenta cast (U and V swapped, or
  NV12 read as NV21) and no colour smear. The copy path's `swap_uv_`
  setting does not apply to the zero-copy path.
- **Motion.** There should be no tearing, no frames from the past (a buffer
  recycled too early), and no freeze while the audio keeps running
  (decoder pool starvation).
- **Seek, scrub, rotary fast-seek, pause/resume, playlist advance, intro to
  menu, movie start/stop, and launch a game then return.** All must behave
  exactly as with the flag off. A freeze right after a seek suggests a v4l2
  flush is blocking on buffers we still hold.
- **CRT modes and bezel.** Run Enhanced CRT (scene FBO and post-processing),
  CRT_NATIVE and Modern TV. Video must sit in the same rectangle as before.

### 4. Colour range and matrix

The two paths will not match pixel-for-pixel. **Some difference is
expected and is a correction, not a regression:**

- **Copy path:** `gst_renderer.cpp` applies BT.601 coefficients to the raw
  Y'CbCr values with **no limited-range expansion**. Black (Y=16) renders as
  dark grey, white (Y=235) as light grey, and HD content (BT.709) gets
  BT.601 colours.
- **Zero-copy path:** the driver converts using the EGL hints from the
  stream's colorimetry. Untagged ≥720-line video is treated as **BT.709
  limited range** and SD as **BT.601 limited range**, which follows
  GStreamer's own defaults.

So with zero-copy, expect deeper blacks, brighter whites, and slightly
different saturated reds and greens on HD content. The things to flag are
**crushed** shadows or highlights (range applied twice, or full-range
content treated as limited), a cast, or SD content that looks clearly wrong.

Compare paths with the kiosk's screenshot hook. It captures the full
composited frame, including the CRT effects:

```bash
DATA=/opt/magic_dingus_box/magic_dingus_box_cpp/data
touch "$DATA/screenshot_request"        # the next drawn frame -> $DATA/screenshots/<UTC>.bmp
ls -t "$DATA/screenshots" | head -1
```

Pause on the same frame (a test pattern or a frame with black, white and
skin tones is ideal) and take a screenshot. Repeat with the flag flipped and
the kiosk restarted. Then compare a few sampled pixels: true black should
read about (0,0,0) with zero-copy versus about (16,16,16) with copy.

## Disable

```bash
sudo systemctl revert magic-dingus-box-cpp     # removes the drop-in
# or: sudo systemctl edit magic-dingus-box-cpp  -> Environment=MDB_VIDEO_ZERO_COPY=0
sudo systemctl restart magic-dingus-box-cpp
```

`systemctl revert` removes **every** drop-in for the unit. If the box has
other overrides, edit the file and delete the one line instead.

## Troubleshooting

| Symptom | Likely cause / next step |
|---|---|
| `copy (missing EGL_EXT_image_dma_buf_import)` | Wrong Mesa or driver stack. Check `EGL_EXTENSIONS` with `eglinfo` (package `mesa-utils-bin`) |
| H.264 still shows `copy (decoder delivered system memory)` | The decoder did not pick DMABuf. Try `GST_DEBUG=v4l2videodec:5,v4l2bufferpool:5` in the drop-in (very noisy, so remove it afterwards) and look for the negotiated caps and io-mode. The decoder's `capture-io-mode` is `auto`, which is expected (not yet verified) to pick dmabuf when downstream asks for the DMABuf feature |
| `eglCreateImageKHR failed (EGL error 0x3003 / 0x300c)` | `BAD_ALLOC` / `BAD_PARAMETER`: offsets, pitch or fourcc were rejected. The WARN names format and size |
| Video freezes, audio continues | Decoder pool starvation. The appsink holds up to 5 samples plus 2 in our ring. Report it with the stats line |
| Stale or wrong frames | A buffer was recycled while still in use. Report the content and its fps |

## Known risks (why this is gated)

- **Never run on hardware.** All the EGL, GL and v4l2 behaviour is untested:
  negotiation, the import itself, v3d's handling of linear NV12/YU12
  imports (it re-tiles them with the TFU, refreshed on every rebind), and
  the colour hints.
- **Buffer starvation and flush.** We hold 2 decoder buffers on top of the
  appsink queue. Fine if v4l2's pool has slack; if it does not, playback
  stalls. Seeks flush the decoder while we still hold buffers (the same
  thing kmssink does), which is untested here.
- **Mixed frames.** If the v4l2 pool copies some frames into system memory
  under pressure, those frames take the copy path and its different colour
  conversion. The result would be a visible colour flicker. The stats line
  shows whether this happens.
- **Permanent fallback keeps DMABuf caps.** After the 30-failure trip, the
  copy path maps the dmabufs on the CPU. That is the same memory the old
  MMAP io-mode path reads, but it is not renegotiated to plain system memory.
- **Pi 5 is untouched** by policy (no hardware H.264, so nothing to import).
  The v4l2 stateless HEVC decoder stays disabled (see `decoder_policy.h`).
