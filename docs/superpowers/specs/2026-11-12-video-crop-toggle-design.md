# Per-video crop toggle — design

Status: DRAFT (2026-11-12), awaiting the owner's approval. The second
overlay style the owner planned is designed in
`2026-11-12-channel-guide-design.md`, which depends on this one; this is
built first. Nothing here is built.

## Intent

Playlist video plays in a 4:3 picture, the product's CRT look. Today the
choice between "crop to fill 4:3" and "show the whole frame" is made once,
at upload, and is permanent: the transcode crops or pads the file and the
original is deleted. The owner wants the choice to belong to the video and
to be changeable while watching:

- Uploads keep whatever aspect ratio they arrive in.
- Cropped-to-fill stays the default for every video.
- A viewer who wants to see one video's whole frame can turn cropping off
  for just that video, from the box itself, using the rotary encoder.

## Goals

- A video can be uploaded at any aspect ratio and stored without cropping
  or padding.
- Each video has one setting, **Crop**: on (default) or off.
  - On: the picture fills the 4:3 area; whatever does not fit is cut off
    equally from both sides (or top and bottom for tall video).
  - Off: the whole frame is visible, scaled to fit, with black bars.
- The setting is changed from the menu overlay on the kiosk and takes
  effect immediately, on the playing video.
- The setting is remembered for that video everywhere it appears (any
  playlist, Master Shuffle) and survives restarts, updates and backups.
- Nothing changes for a box whose library is all 4:3 files: same picture,
  same menu.

## Non-goals

- Recovering the cut-off edges of videos uploaded before this change. Those
  files were cropped at upload and the originals were deleted; they stay as
  they are. Only a re-upload brings the edges back.
- Pan/zoom, per-video crop position, or any crop other than centered.
- Media Browser (movie/TV) playback. It already preserves aspect and is
  untouched.
- Games.

## Anti-goals

- A video must never be shown stretched or squished. (Today a non-4:3 file
  that reaches a playlist without going through the transcode is stretched
  into the 4:3 area. This design removes that.)
- The toggle must not be reachable by accident in a way that is hard to
  undo, and must never interrupt or restart playback.
- No new stalls on the render thread (see Constraints).

## Decided by the owner

1. Crop is the default.
2. The control lives in the menu overlay (the playlist list shown over a
   playing video).
3. Placement: it is reached by turning the encoder **down past the last
   playlist**. Focus then leaves the list and lands on a control in the
   bottom-right now-playing block, near the video title (to its left).
4. Form: a checkbox labelled Crop. Ticked = cropped; empty box = not
   cropped. One press toggles it.
5. It is per video: one video inside a playlist can be set to no cropping
   without affecting the others.

## Behavior

### Overlay

The overlay today: playlist list on the left (row 0 is Master Shuffle),
and while a video plays a now-playing block bottom-right (title, artist,
elapsed / duration). Encoder turn moves the list cursor, clamped at both
ends; encoder press on the playing playlist hides the overlay, on another
playlist switches to it.

New:

- While a video is playing and Crop would make a visible difference for it
  (see "When the control appears"), the now-playing block shows the Crop
  checkbox with its current state, whether or not it has focus.
- Turning down from the last playlist row moves focus to the checkbox. The
  list then shows no highlighted row. Turning up returns focus to the last
  playlist row. Turning down again does nothing (clamped).
- Pressing while the checkbox has focus toggles Crop for the playing
  video. The picture changes at once; the overlay stays open; playback is
  not interrupted. It never switches playlist and never hides the overlay.
- Every input that drives the list today drives this identically: rotary
  encoder, gamepad/keyboard up-down and select, phone remote.
- If the video changes while the checkbox has focus, the checkbox shows
  the new video's state. If the new video has no checkbox, focus returns
  to the last playlist row.

### When the control appears

Shown only when all hold: a playlist video is playing (not the intro, not
a game, not Media Browser), the overlay is visible, and the video's
display aspect ratio differs from 4:3 by more than a small tolerance.
For a 4:3 video the two settings produce the same picture, so there is no
control and no extra stop below the last playlist. [builder's call —
proposed tolerance 2%; the owner has not weighed in on hiding vs. showing
it disabled]

### In the Channel Guide style

When the owner picks the Channel Guide overlay instead of the list
(`2026-11-12-channel-guide-design.md`), the same checkbox sits at the
bottom of the guide's now-playing panel, which is above the channel grid.
It is reached by turning UP past channel 00 rather than down past the last
row, and behaves identically otherwise. [Claude's call — keeps the motion
spatial; the owner has not weighed in]

### Picture

"The 4:3 area" is what the kiosk already uses for playlist video: in
Modern TV mode a centered 4:3 rectangle (960x720 at 720p, 1440x1080 at
1080p); in CRT Native mode the whole framebuffer, which the TV or
HDMI-to-composite converter presents as 4:3 — so there the framebuffer's
pixels are not square and the math must work in display aspect, not pixel
counts.

- Crop on: scale the video to cover the 4:3 area, centered; the overflow
  is not drawn. Nothing may be drawn outside the 4:3 area (the pillarbox
  bars in Modern TV mode stay black).
- Crop off: scale the video to fit entirely inside the **visible opening**,
  centered, black elsewhere. With a bezel on (Modern TV mode), the visible
  opening is the bezel's transparent window, which is smaller than the 4:3
  area — measured on the shipped art, about 1348x984 inside a 1440x1080
  area for the three MDB bezels (roughly 45 px hidden per side) and about
  1403x1045 for the other eight (retro_tv_1 and the rest share one
  opening); every opening is centred. Without a bezel, and in CRT Native mode, the
  visible opening is the 4:3 area.
- Pixel aspect ratio from the stream is honored (the renderer already
  reads it for the Media Browser path).
- The intro video and anything without a stored setting behave as Crop on.

### Upload

- The transcode stops cropping and padding. It scales down only, never up,
  preserving the source aspect, to the smallest size at which Crop-on
  playback is not upscaled for the chosen quality preset — i.e. the source
  scaled to *cover* the preset's 4:3 box — and never outside a 1920-wide ×
  1080-tall box, so a 9:16 portrait clip is stored at about 608×1080.
  [Claude's call: stays inside what the Pi 4B's hardware H.264 decoder is
  known to handle; storing portrait up to 1080×1920 is worth revisiting only
  after a Pi 4B test shows it decodes in hardware]
- The `modern` preset (1280×720, 16:9) no longer differs from `crt_hd` once
  nothing is cropped or padded: the API keeps accepting it as an alias of
  `crt_hd`. The Quality picker already offers only `crt_hd` and `crt`.
  [Claude's call]
- Rotation metadata is respected: a phone clip stored sideways with a
  rotate tag is sized by its displayed dimensions.
- The Content Manager's upload "Framing" choice (Fill screen / Show
  everything) stays, but now sets the new video's initial Crop setting
  instead of altering the file.
- Both upload paths (the file picker and drag-and-drop / retry) send the
  same options. Today the drag-and-drop path silently ignores Quality,
  Framing and Normalize.
- Cost to state in the UI help text or release notes: a 16:9 upload at the
  default quality is stored at 1280x720 rather than 960x720, about a third
  more pixels and correspondingly larger files.

### Content Manager

Each video in the library shows its Crop setting and can change it. This
is the same setting the kiosk toggles; a change on either side shows on
the other. [builder's call — the owner asked for the kiosk control; the
web control is this design's addition]

## Where the setting is stored

[builder's call — the owner decided "per video"; the mechanism is this
design's]

A small per-video **media info** store of its own in the data directory,
keyed by the video's path relative to the data directory. Each entry holds
the crop override (absent = the default, cropped) and, because the Channel
Guide design needs every video's length before it plays, `duration_s` and
`display_aspect`, which the web admin writes when it saves a file and
back-fills once for videos already on a box. The kiosk reads
`display_aspect` from it, so it knows whether to offer the checkbox before
the stream has prerolled. Not in the playlist YAML, because:

- the kiosk has never written playlist files, and should not start;
- the playlist editor and package import currently delete any per-item
  field they do not know (three hand-kept field lists);
- the owner's rule is per video, and a video can sit in several playlists
  and in Master Shuffle.

Requirements:

- Both the kiosk and the web admin read and write it. Writes are atomic
  (temp file + rename) and serialized between the two processes with a
  file lock, the pattern the phone text queue already uses.
- One schema of record: a shared fixture file read by both a C++ test and
  a Python test, as `settings.json` already has.
- A missing, empty or corrupt store means "every video is cropped" and
  must not block playback or the menu.
- It is included in backup/restore and playlist package export/import (a
  packaged video carries its setting), excluded from deploy and OTA rsync
  deletes like the other per-box data files, and wiped on clones if the
  shipped library is curated. Deleting or renaming a video in the Content
  Manager drops or moves its entry.

## Constraints

- **Render thread.** The kiosk's main loop pings a 10 s systemd watchdog.
  The toggle's file write must not run on it (SD-card writes have been
  measured at seconds under load). The in-memory setting changes
  immediately; the write happens on a background writer. Nothing in the
  focus or toggle path may sleep or block.
- **Both boards, one image.** No board-specific code; the Pi 4B (hardware
  H.264 up to 1920x1080, no spare CPU) is the envelope.
- **Redraw gate.** A toggle and a focus move must each cause a redraw.
  While a video is playing the loop already draws every frame; the focus
  state still belongs in whatever the main menu's redraw decision reads.
- **Layout.** The checkbox must not overlap playlist text at either
  logical canvas (640x480 CRT Native, 960x720 Modern TV), including with a
  long title. If it does not fit to the left of the title it goes on its
  own line in the same block. [builder's call]
- **Tests on the Mac.** The picture math and the overlay focus rules are
  pure functions with unit tests; neither may need GL, DRM or GStreamer.

## Shape of the work (for the builder; not decided by the owner)

- One pure placement function replaces the 4:3 rectangle math that
  currently exists three times (`main.cpp` physical rect, `main.cpp`
  logical rect, `GstRenderer::render_quad`): given source display aspect,
  target rect, target display aspect, visible opening and crop on/off, it
  returns where to draw and which part of the frame to sample.
- One pure overlay-navigation unit replaces the inline list navigation in
  `main.cpp` (cursor move, scroll window, clamp — the clamp is duplicated
  in `playlist_reload.cpp`) and adds the "focus below the list" state.
  `PlaylistPlayback::on_select` gains the "focus is on the checkbox"
  branch ahead of its existing ones.
- Each bezel declares its visible opening in `bezels.json`; a test checks
  the declared rectangle against the PNG's transparent region. The
  procedural bezel computes its own.
- The video's display aspect comes from the media info store; for a video
  the store has no entry for yet, from the stream once it has prerolled
  (until then the checkbox is absent).

## Open, for the owner

- A box with many playlists means many detents to reach the checkbox from
  the playing playlist. Accepted as-is unless the owner wants a shortcut.
- Hiding the checkbox for 4:3 videos (proposed) versus always showing it.
