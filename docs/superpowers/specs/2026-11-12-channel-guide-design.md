# Channel Guide overlay — design

Status: DRAFT (2026-11-12), awaiting the owner's approval. Nothing is built.
Mockups: the "Channel Guide Overlay" design canvas
(https://claude.ai/artifact/RycvJje38RHqb8rNt7SJaT — private to the owner).
Depends on: `2026-11-12-video-crop-toggle-design.md` (the per-video store
and the Picture control), which is built first.

## Intent

The overlay that appears over a playing video today is a list of playlists.
The owner wants an alternative that feels like an old cable-TV channel guide:
every playlist is a channel, every channel is "on" all the time, and a grid
shows what is playing now and next across all of them, with the current
picture in a window at the top. It is chosen in Settings; the list stays the
default.

What makes it special is the illusion of live TV on a box that has no live
TV: you are not picking a playlist to start, you are tuning into a channel
that was already running.

## Goals

- A second overlay style, **Channel Guide**, selectable in Settings → Display
  as "Menu Style: List / Channel Guide". List remains the default and behaves
  exactly as today.
- In Channel Guide style, playlists behave as **live channels**: each loops on
  a schedule anchored to wall-clock time, and tuning in joins whatever is on,
  at the right second.
- The guide screen follows the mockup's layout **A · Picture window**: the
  tuned channel's video in a 4:3 window top-left, a now-playing panel
  top-right, and a channel grid across the bottom half.
- It looks like part of the existing kiosk, not a bolt-on: same fonts, palette,
  panel treatment, CRT effects and bezels, and the Magic Dingus Box logo stays
  on screen.
- Fully usable with the rotary encoder alone (turn + press), and equally with
  a gamepad, keyboard and the phone remote.
- Works on both boards and both display modes (Modern TV 960×720 logical
  canvas, CRT Native 640×480).

## Non-goals

- Previewing another channel's video while browsing (one decoder; the window
  always shows the tuned channel).
- Game playlists as channels. Games stay in Settings → Video games.
- Media Browser (movies/TV) as channels.
- A DVR: no rewind past the start of the current video, no catching up on
  what you missed.
- Real program metadata (ratings, descriptions, genres) — the item's title and
  artist are all there is.
- Changing how the List style or the Content Manager's playlist editor work,
  beyond what this needs (durations, the setting).

## Anti-goals

- The guide must never stall playback or the render thread (systemd watchdog,
  10 s). Building the schedule and the grid is pure arithmetic on data already
  in memory — no file or network access per frame.
- Tuning must never feel like "starting a file": no visible jump to 0:00, no
  black gap longer than today's playlist switch.
- A box that never touches the setting must not change at all. (The
  system time zone is the one exception, by decision: see "Clock", the
  2026-11-13 clarification.)
- No unreadable grid: no cell text below the kiosk's 14 px body size on the
  720-tall canvas, nothing outside the bezel's visible opening.

## Decided by the owner

1. Channel Guide is an **option**; the existing list overlay stays the default.
2. **Join like live TV**: tuning in joins mid-video at the scheduled position.
3. Layout **A · Picture window** (video window top-left, info top-right, grid
   bottom). Layout B was shown and not chosen.
4. The **logo** must be on the guide screen (mockup: bottom-right of layout A,
   level with the control hints).
5. It must read as an **extension of the current design system**.
6. Accepted as stated back to the owner (2026-11-12), not separately argued:
   - live behaviour comes with the Channel Guide style; List keeps today's
     on-demand behaviour;
   - in Channel Guide style the yellow (PREV) and green (NEXT) buttons, and a
     gamepad's L/R, are **channel down / channel up**;
   - seeking and pause still work within the current video; when that video
     ends, playback rejoins the schedule at whatever is on now;
   - turning the box on (after the intro) tunes to the **last channel
     watched**, live, instead of the empty menu.
7. **Nothing playing is never a resting state** (answered 2026-11-12):
   whenever the kiosk would otherwise land on the menu with nothing playing
   — power-on, the end of a game, leaving the Media Browser, a channel that
   gave up after repeated failures — it re-tunes live. A box that has never
   watched a channel starts on **channel 01**. The owner accepted that sound
   then starts by itself the moment a game or movie ends.
8. **Time zone comes from the owner's phone or computer** (answered
   2026-11-12): the box sets its time zone from the browser that opens the
   Content Manager. No new kiosk screen.

## The schedule ("live" model)

- A channel is a video playlist (the same set and order the List style shows,
  after `split_for_ui_with_master_shuffle`). Channel numbers are the list
  indices: **00 = Master Shuffle**, 01… = playlists in list order.
- Each channel's items play back to back and loop forever, starting at a fixed
  epoch (proposed: 2026-01-01T00:00:00Z — the value is arbitrary but must never
  change, or every box's schedule shifts). The playlist's `loop:` key is
  ignored in Channel Guide style; a channel always loops.
- An item's scheduled length is its playable length: `end − start` when the
  item has trims, else the file's duration. Lengths come from the per-video
  store (below), never from probing during playback.
- What is on channel C at time t: `(t − epoch) mod total(C)` locates the item
  and the offset into it. This is a pure function of (items, lengths, t) and is
  the unit-tested heart of the feature.
- **Master Shuffle** in Channel Guide style is a fixed, deterministic mix of
  every video channel's items (a seeded shuffle, stable until the library
  changes), scheduled the same way. Its List-style behaviour (fresh shuffle,
  history) is untouched.
- Editing a playlist (adding, removing, reordering, re-trimming) changes that
  channel's schedule from then on. Accepted: the guide is recomputed on the
  existing `playlists_reload_request` path.
- An item whose length is not yet known is **left out of the schedule** until
  it is known (a channel whose items are all unknown shows "No schedule yet" in
  its row and cannot be tuned). [builder's call: the exact empty-row wording]

### Tuning and staying on schedule

- Tune = load the scheduled item and start it at the scheduled offset (the
  `load_file` start parameter Media Browser resume already uses; playlist
  loads gain an offset to reach it).
- Channel up/down goes through the same non-blocking switch as a tune. While
  the guide is open it stays open and the cursor follows; a press to tune
  closes it.
- The tuned channel is its own state: during Master Shuffle the playing
  playlist is the source playlist, not channel 00. The guide's cursor and
  scroll are its own too (the List style's row count does not apply).
  The playlist-switch state machine in `PlaylistPlayback` (stop → settle →
  load, no sleeping on the render thread) is reused; the offset is computed
  when the load actually runs, not when the button was pressed, so the settle
  time does not put playback behind.
- When an item ends, the next item is loaded at its scheduled offset (normally
  a fraction of a second in). Small drift from load times is therefore
  corrected at every item boundary; no mid-item resync.
- After a **seek** or a **pause**, the current item carries on from where you
  left it; at its natural end the channel rejoins the schedule (which may skip
  part of the next item, or the whole of it — that is live TV).
- A tuned item that fails to load is handled by the existing failed-item policy,
  except that "skip" means "wait for the next scheduled item" rather than
  "load the next item now". [builder's call: how that looks on screen]

### When nothing would be playing

Every path that today leaves the main menu with no video re-tunes live in
Channel Guide style (owner decision 7). There are more of those paths than
the four named in the decision (power-on alone has four: the intro ending,
no intro file, an intro that fails to load, an intro that times out), so this
is ONE rule evaluated every frame, not a hook per path: in Channel Guide
style, on the main screen, once the intro is over, with no video playing, no
switch pending, no game loading and the post-game display restore finished,
tune. The target is the last channel watched, else channel 01, else the
lowest channel with a schedule.

- Give-up is the exception to "the last channel": re-tuning the channel that
  just failed repeatedly would loop. After a give-up the kiosk tunes the next
  channel up that has a schedule, with the existing error toast. If no
  channel can play, it shows the guide with the video window dark and the
  toast — the only time the guide is shown with nothing tuned. [Claude's
  call, within decision 7]
- The guide is not opened by these re-tunes; the video simply comes back,
  as when a TV is switched on.
- After a game the video always comes back full screen, even when the
  viewer had the guide open when they launched it (Settings over the
  guide → Video Games): a game session takes the guide back from the
  viewer, so the post-game re-tune closes it. [owner decision,
  2026-11-13, after the Pi 5 test]
- List style keeps today's behaviour on all of these paths.

### Clock

- The schedule uses the system's wall clock. Boxes get time over the network
  (NTP). Offline, the schedule still runs and stays self-consistent; only the
  times printed in the guide can be wrong.
- A clock jump (NTP sync after boot, DST is not a jump — schedule math is in
  UTC, display in local time) never interrupts what is playing; the new time
  applies at the next tune or item boundary.
- **Time zone** (owner decision 8): nothing sets a box's zone today, so every
  unit would print the golden-image source box's local time. The Content
  Manager's page reports the browser's IANA zone
  (`Intl.DateTimeFormat().resolvedOptions().timeZone`) to the web admin when
  it loads; the web admin validates it against the system's zoneinfo and,
  only when it differs from the box's, applies it with `sudo -n timedatectl
  set-timezone` (the web admin already runs root commands through the box's
  passwordless sudo, as Box health does; there is no narrower rule to copy),
  and rewrites the `TZ=` line of `services/.env` when that file exists (the
  media containers and, through its `EnvironmentFile=`, the kiosk read it).
  Requirements:
  - the kiosk picks the new zone up without a restart: it drops any inherited
    `TZ` at start-up so `/etc/localtime` decides, and calls `tzset()` when the
    web admin pokes a `timezone_reload_request` marker (glibc's
    `localtime_r` does not re-read a changed `/etc/localtime` by itself);
  - the latest browser to connect wins; no prompt; [Claude's call]
  - clones keep the image's zone until their first Content Manager
    connection, rather than resetting to UTC; [Claude's call]
  - the zone is per-box state: excluded from backup/restore, which moves
    content between boxes.
  - **Clarification (2026-11-13), against the anti-goal "a box that never
    touches the setting must not change at all":** the browser's zone is
    applied — and `services/.env` `TZ=` rewritten — on EVERY box, List
    style included, not only on boxes in Channel Guide style. A correct
    system clock zone is right for the whole box (logs, the media
    containers, anything that prints local time), so this is kept
    deliberately; the anti-goal covers what the kiosk shows and does, not
    the system zone.
  - A browser that reports UTC to hide its zone (Firefox
    resistFingerprinting, Tor, Brave's strict mode) never moves a box
    already on a named zone: a UTC report then changes nothing. [Claude's
    call, 2026-11-13 final review]

## Controls (Channel Guide style)

| Input | Guide closed (video only) | Guide open |
|---|---|---|
| Encoder turn | seek (as today) | move the cursor between channel rows |
| Encoder press / SELECT | open the guide | on another channel's row: tune to it and close the guide, as a playlist switch in the List style does today; on the tuned channel's row: close the guide; on the Picture control: toggle crop (the guide stays open) |
| Yellow PREV / green NEXT, gamepad L/R | channel down / up, live | channel down / up, live (cursor follows) |
| Red PLAY_PAUSE | pause / resume (as today) | same |
| Black BTN4 | Settings (as today) | Settings (as today) |
| Phone remote | same mapping (its buttons already produce these actions) | same |

- The cursor starts on the tuned channel each time the guide opens.
- Tuning closes the guide (the reused switch logic already hides the
  overlay when a switch finishes). [Claude's call: matches the List style
  and a cable box; browsing several channels means reopening the guide]
- The guide never closes by itself, like the list overlay today (its
  3-second `ui_visibility_timer` is set but read nowhere). [Claude's call;
  cheap to change later]
- Turning up past channel 00 moves the cursor onto the **Picture control** in
  the now-playing panel (the crop checkbox from the crop design). It is only
  there when the playing video is not 4:3 (rule from the crop design). Turning
  down from it returns to channel 00. [Claude's call: the crop design puts the
  control past the LAST row of the List overlay, where the now-playing text
  sits; in the guide the now-playing panel is ABOVE the grid, so "past the top"
  keeps the motion spatial. The owner has not weighed in.]
- More channels than fit (5 rows at 720, 5 at 480): the grid scrolls to keep
  the cursor row visible, with the existing up/down arrow treatment.
- Channel up/down wraps from the last channel to 00 and back (a TV remote
  does). [Claude's call]

## Screen (layout A)

Geometry from the mockup, in logical-canvas px (Modern TV 960×720 / CRT
Native 640×480). Everything stays inside the bezel's visible opening
(≈ 30 px in from left/right and 32 px from top/bottom at 960×720 with the
MDB bezels).

| Element | 960×720 | 640×480 |
|---|---|---|
| Video window (4:3) | x40 y40 384×288 | x24 y24 224×168 |
| Now-playing panel | x448 y40 472×288 | x260 y24 356×168 |
| Grid | x40 y352 w880; header 30, rows 52 | x24 y206 w592; header 24, rows 40 |
| Channel column | 196 wide | 132 wide (short names) |
| Time window / ticks | 20 min, tick every 5 | 15 min, tick every 5 |
| Hints + logo row | y≈660; logo 181×26 right | y≈446; logo 139×20 right |

- **Video window**: the tuned video rendered into the window rect via
  `GstRenderer::set_render_inset` (already used for Media Browser playback),
  framed by a 1 px `fg` hairline at 18 %. A small channel "bug" (Zen Dots,
  number, 70 % fg) top-left inside it. The crop setting applies inside the
  window exactly as it does full-screen.
- **Now-playing panel**: `bg_lift` fill with a 3 px gold rule on top (the
  Marquee panel treatment). Rows: eyebrow ("NOW PLAYING" green, or "ON CH 04
  NOW" gold when the cursor is on another channel) + clock (Zen Dots) ·
  CH box (gold Zen Dots number in a gold-hairline box, "CH" label; "MIX" for
  00) + playlist title (Zen Dots) · rule · item title (mono bold) + artist
  (dim) · start time, gold progress bar, end time · "NEXT" (steel blue) + time
  + next title · bottom row: the Picture control and "16:9 SOURCE" when on the
  tuned channel, or "PRESS TO WATCH" + "joins at m:ss" when the cursor is on
  another channel. At 640×480 the artist and NEXT rows and the source label
  are dropped.
  On the tuned channel the panel describes the picture: the progress bar
  and start/end times come from the player's position and the item's
  length — also when the playing item IS the on-air airing (a join's seek
  lands on the keyframe before its target, so the picture can run up to a
  GOP behind the schedule) — while NEXT stays the schedule's next airing.
  [owner decision, 2026-11-13, after the Pi 5 test]
- **Grid**: header row = date (dim) + time ticks (steel blue, first with
  AM/PM). Each row = channel cell (number in Zen Dots, or the shuffle icon for
  00; short name in mono caps — on CRT Native in 12 px, from x 36 with a
  4 px right margin, so ~12 letters fit the 132 px column instead of 9;
  owner decision 2026-11-13 after the Pi 5 test) + program cells positioned on the time axis,
  3 px gaps showing the background as grid lines. A cell narrower than 30 px
  shows no text; narrower than 64 px shows no artist; a program that began
  before the window starts with "◂". A red (`highlight2`) "now" line with a
  small marker in the header crosses all rows.
- **Colours** (all existing tokens): background `bg`; cells and panel
  `bg_lift` (the only off-background fill); text `fg` / `dim`; **gold** =
  cursor (4 px bar on the channel cell, gold number, 2 px gold ring on the
  cursor row's on-now cell, gold Picture focus); **green** = the tuned
  channel (number, name, on-now title); **steel blue** = time axis and NEXT;
  **red** = the now line only.
- **Type**: Zen Dots for clock, channel numbers, playlist titles; DejaVu Sans
  Mono for everything else; sizes from the existing theme (14 / 18 / 22 …),
  with 13 px only for the hint row and small labels as in the mockup, and
  12 px for the CRT canvas's channel names (the 14 px floor below binds the
  720-tall canvas).
- **Hints row**: knob icon + "TURN channels", "PRESS watch", "PRESS on green to
  hide guide" (TV); first two only on CRT. Logo at the right end.
- CRT effects (scanlines, warmth…) and the bezel apply exactly as to the list
  overlay.
- The clock, progress bar and now line move: while the guide is open the redraw
  gate must keep drawing (a video is always playing in this style, so it
  already does; the guide's state goes in the redraw signature regardless, per
  the redraw-gate rule in CLAUDE.md).

## Data the kiosk needs

- **Every video's playable length**, before it is ever scheduled. The crop
  design's per-video store becomes the per-video **media info** store, keyed by
  the video's path relative to the data directory, holding `duration_s`,
  `display_aspect` and the crop override. The web admin writes `duration_s` and
  `display_aspect` when it saves a file (it already runs `ffprobe` on upload);
  a one-time background job fills them in for videos already on a box (run by
  the web admin on start, low priority, never during a game, idempotent). The
  kiosk only reads it, at boot and on the existing reload paths — never per
  frame. Same atomicity, locking and schema-fixture rules as the crop design.
- **Last channel watched**: persisted in `settings.json` with the setting, via
  the existing background settings writer.
- **Menu style**: `display_settings.menu_style` (`list` | `channel_guide`) in
  `settings.json`, default `list`, included in backup/restore like every other
  display setting.

## Where it lives in the code (for the builder; not decided by the owner)

- Pure, Mac-tested units (no GL/DRM/GStreamer): the schedule (`what is on C at
  t`, `next boundary`), the Master Shuffle mix, the grid layout (cells, ticks,
  now-line x, text visibility) for both canvases, and the guide's navigation
  state (cursor, Picture focus, scroll, channel up/down wrap). Follow the
  "spawn, drain and paint" rule: decisions in these units, the renderer paints.
- `Renderer` gains a `render_channel_guide` beside `render_playlist_list`,
  selected by the menu style; `main.cpp`'s viewport code sets the video inset
  when the guide is open.
- `PlaylistPlayback` gains the live tune / channel step / rejoin-at-boundary
  behaviour behind the menu style, reusing its switch state machine and fake
  transport tests; List style code paths stay as they are.
- Settings → Display gets the "Menu Style" row (`settings_menu.cpp`), Gold
  section colour as the Display submenu already uses.

## Constraints

- Render thread: no blocking I/O; the schedule and grid are computed from
  in-memory data (bounded: ≤ a few hundred items × the rows on screen).
- Both boards from one binary; the Pi 4B is the envelope (rendering the video
  into a sub-rect costs nothing extra — the window is smaller than the screen).
- Both display modes, both canvases; bezel-safe.
- Unit tests for every pure unit above; the renderer is checked on hardware.
- New settings keys go through the existing settings persistence, fixture and
  backup paths.

## Left to the builder

The exact epoch value, the "no schedule yet" wording, how a failed tune looks,
the shuffle seed, animation (if any) when the guide opens, and anything in the
geometry the mockup does not pin down — keeping the mockup's look.

## Open, for the owner

Nothing. Decisions 7 and 8 closed the last two.
