# Movie Channels, Daily Lineups and Pick-Ahead — Design

Status: draft for owner approval (2026-11-13; builder check done, owner
answered framing and downloads). Builds on the Channel Guide
(`2026-11-12-channel-guide-design.md`, branch `channel-guide`, PR #16) and
the Media Browser. Owner decisions are marked **(owner)**; calls made on
the owner's behalf are marked **(Claude's call)** — the owner may overturn
any of them.

## Why

The Channel Guide turns playlists into live channels. The owner wants the
box's downloaded movies to feel the same way: flip to a "Comedy" or "Kids"
channel and a movie is already playing, like cable. Two things make that
feel real rather than mechanical: the lineup should not be the same every
day, and a viewer who wants control should be able to look ahead in the
guide and start something now. **(owner)**

## Goals

1. **Movie channels.** When the Media Browser is unlocked, the movies on
   the box's movie drive become channels in the guide, organized
   automatically into types from what is in the library — older films
   become Classics, sci-fi becomes Sci-Fi, kids' films a Kids channel —
   and the set rebuilds itself as the library changes. **(owner)**
2. **Daily lineups.** Every channel (playlist and movie) gets a new
   order each day, so the same title does not always lead or always follow
   the same title. **(owner: rotation; Claude's call: once a day,
   overnight — see below)**
3. **Pick-ahead.** Besides "true live", a setting where the viewer can
   move along a channel's row into later programmes and start one now.
   **(owner)**

## Non-goals

- TV episodes from Sonarr as channels (the owner's library has none on
  disk today; a later step). **(Claude's call)**
- Editing or naming movie channels by hand, or channel rules in the
  Content Manager. The system is automatic. **(owner: "dynamic")**
- Movies in the List style. List style is unchanged.
- Recording, per-viewer profiles, parental PINs.

## Anti-goals (a failure even if it "works")

- The Kids channel ever showing a PG-13, R, NR or unrated film.
- The owner's movie titles shipped in a golden image or synced to other
  boxes (CLAUDE.md "Inherited source-box state"; same rule as
  `media_info.json`).
- File I/O or a Radarr call on the render loop (10 s watchdog).
- Torrents left paused when no movie is playing; or a movie stuttering
  because downloads were not paused on a 2 GB board.
- A tune that stops on a movie whose file is gone (drive unplugged) and
  shows a dark window instead of moving on.
- Any change to List style, or to Channel Guide behaviour when the Media
  Browser is locked (apart from daily lineups).

## Part 1 — Movie channels

### Where the movie list comes from
- The kiosk already lists the library: `RadarrClient::get_library_checked()`
  (`/api/v3/movie`). Only movies with a file are used. The parser gains
  `genres`, `certification` and the file's exact length (`movieFile.
  mediaInfo.runTime`, `H:MM:SS` — seconds matter for a schedule) and the
  video codec (`movieFile.mediaInfo.videoCodec`). Paths go through the
  existing `RadarrClient::resolve_host_path`.
- Radarr is only reachable while the VPN tunnel is up. The kiosk fetches
  the list on a background worker at start-up (once the tunnel is healthy
  per `VpnHealthMonitor`) and every 15 minutes after, and saves a snapshot
  (`data/movie_library.json`, atomic write, worker thread) that it loads at
  start-up. Movie channels therefore keep working through a tunnel outage
  and across restarts. **(Claude's call: 15 min)**
- The snapshot is private content: excluded from deploy/OTA rsyncs, pruned
  or removed by golden-image curation and deleted on first boot — the same
  handling `media_info.json` already gets. **(Claude's call, required by
  the anti-goal)**
- A movie whose file is missing at refresh, or whose drive is not mounted,
  is left out; if `/mnt/ssd` is not mounted, there are no movie channels.
  A failed movie load also triggers an immediate drive check on the
  worker, so an unplugged drive removes movie channels at once rather
  than at the next 15-minute refresh. **(Claude's call, builder check)**
- **A movie that fails to load never leaves a dark window.** The Channel
  Guide's failed-item rule ("wait for the slot to end") would mean up to
  two hours of black on a film. For movie channels a failed item instead
  starts the channel's next programme now, from its beginning (ahead of
  the schedule; the boundary logic re-joins the schedule later); the
  failure still counts toward the channel's give-up budget. **(Claude's
  call, builder check)**
- Pi 4B: a movie the board cannot decode in hardware (HEVC, AV1 — the Pi
  4B has no spare CPU for software decode, CLAUDE.md dual-board contract)
  is left out of channels on that board, via a `PlatformProfile` field.
  The quality profile makes these rare. **(Claude's call)**

### How channels form (pure, tested unit)
- A channel appears only when at least **3 movies** qualify; at most
  **8 movie channels**, keeping the fullest (Movie Mix always kept).
  **(Claude's call, owner agreed to the proposal)**
- Candidate channels, in this fixed order (numbers stay stable as the
  library grows; a channel that drops out just closes its gap):

  | Channel | Rule |
  |---|---|
  | MOVIE MIX | every movie |
  | KIDS | certification G or PG **and** a genre of Family or Animation |
  | CLASSICS | year before 1980 |
  | COMEDY | Comedy |
  | DRAMA | Drama |
  | ACTION & ADVENTURE | Action or Adventure |
  | SCI-FI | Science Fiction or Fantasy |
  | THRILLERS & CRIME | Thriller, Crime or Mystery |
  | HORROR | Horror |
  | ROMANCE | Romance |
  | WAR & HISTORY | War, History or Western |

  A movie can be on several channels. Genre names are TMDB's as Radarr
  reports them. **(owner: organize by type, older = classics, sci-fi,
  kids; Claude's call: the table)**
- Movie channels are numbered after the playlist channels. Their
  `last_channel` identity is `movies:<type>` (e.g. `movies:kids`), so
  power-on returns to the same movie channel even if its number moved.

### How a movie channel behaves
- Scheduled like any channel (daily lineup, Part 2) with each movie's
  real file length. The guide shows title and year (the year where a
  playlist item shows the artist).
- **Framing: edge to edge.** A movie channel playing full screen looks
  like Media Browser playback — the full widescreen, **no 4:3 pillarbox
  and no bezel** — while playlist channels keep the CRT frame. The film
  shows whole (letterboxed) by default; the Picture control on the tuned
  panel can switch a film to **fill the screen** (crop to the output's
  shape) and remembers it per film (the per-video store gets a movie key,
  e.g. `movie:<tmdb_id>`, whose default is "whole picture"). With the
  guide open, the guide keeps its normal look and the picture window shows
  the film whole. In CRT Native the output is 4:3, so "edge to edge" is the
  4:3 screen. **(owner: edge to edge, 2026-11-13; Claude's call: the
  details)**
- **Overnight download window ("off the air").** From **1:00 AM to
  6:00 AM** local time movie channels are off the air: their rows read
  OFF THE AIR · BACK AT 6:00 AM, they cannot be tuned, and a movie still
  playing at 1:00 AM ends there. A box on a movie channel at 1:00 AM moves
  to channel 00 (Master Shuffle) — without changing the remembered last
  channel — and returns to that movie channel at 6:00 AM if the viewer has
  not chosen another channel since. Downloads therefore always get a
  nightly window even on a box left on a movie channel. Each movie
  channel's day runs 6:00 AM → 1:00 AM. The Media Browser's own playback is
  unaffected (a deliberate choice by the viewer). **(owner: catch up
  overnight, 2026-11-13; Claude's call: 1–6 AM, Master Shuffle as the
  fallback, the return at 6 AM)**
- **Downloads:** while a movie from a movie channel is playing, the same
  service quiet mode the Media Browser's PlaybackScreen uses applies
  (`platform::service_quiet_mode`: full pause on Pi 4B and 2 GB boards,
  trickle on 4 GB+), released as soon as the playing item is not a movie
  (channel change, game, Media Browser entry, nothing playing). It must
  coexist with PlaybackScreen's own consent records — never unpause what
  the Media Browser paused or the owner paused. **(owner agreed)** The
  release waits a short grace period (about 2 minutes) after the last
  movie stops, so channel surfing past movie channels does not stop and
  start the containers every few seconds (`playback_services_pause.sh`
  takes 20–40 s per cycle on a Pi 4B). **(Claude's call, builder check)**
- **Watch history:** channel viewing does not mark movies watched or move
  the Media Browser's resume point. **(Claude's call, owner agreed)**
- Movie channels exist only when the Media Browser is compiled in and
  unlocked, and only in Channel Guide style. Locking the Media Browser
  removes them at the next channel rebuild.

## Part 2 — Daily lineups (all channels)

- Lineups change **from day to day at 4:00 AM local time**, and are fully
  determined by the channel, the date and the time — so the guide's "later
  tonight" is the real plan and stays the same across restarts.
  **(owner: rotate between sessions so the same title isn't always first;
  Claude's call: daily at 4 AM — a per-power-on reshuffle would rewrite the
  guide's future every time the box starts, which breaks pick-ahead)**
- **Playlist channels keep their written order but start somewhere
  different each day** (the day's lineup is the playlist rotated to a
  date-seeded starting item, looping in order). The same video is not
  always first, and a curated sequence keeps its order. **(Claude's call,
  builder check: a full shuffle would break playlists whose order matters,
  and an opt-out would need a playlist key and Content Manager changes)**
- **Movie channels, Master Shuffle and Movie Mix are fully shuffled**, and
  reshuffled on **every pass** through the channel (seeded by channel
  identity, date and pass number), so a short channel does not repeat one
  order all day. **(Claude's call, builder check)**
- Shuffles use the same hand-written Fisher–Yates over `std::mt19937` the
  Master Shuffle uses (identical on the Mac and the Pi).
- Each day's lineup starts at that day's 4:00 AM (movie channels: 6:00 AM,
  after the off-air window); an item still running at the boundary is cut
  there (as TV schedules do). Time-zone changes move the boundary with the
  clock.
- List style keeps playing playlists in their written order.

## Part 3 — Pick-ahead

- Settings → Display gains **Guide Mode: Live / Pick Ahead** (default
  Live), shown only in Channel Guide style. **(Claude's call: name and
  place)**
- **Live** is today's behaviour exactly.
- **Pick Ahead** adds moving along a channel's row in time:
  - Knob or encoder only (the physical box has no left/right): a press on
    a channel row enters that row with the cursor on the programme on now;
    turning moves along the row's programmes (the grid scrolls in time,
    up to 24 hours ahead); a press starts the highlighted programme; the
    black/menu button, or turning back before "now", leaves the row.
  - Phone remote and keyboard: LEFT/RIGHT move along the row directly.
  - A press on the programme on now tunes live, as in Live mode.
  - **(Claude's call: the interaction; it keeps Live mode's "press tunes")**
- Starting a later programme plays it **from its beginning, now**; the
  panel eyebrow reads **ON DEMAND**. The guide's rows keep showing the real
  live schedule.
- When an on-demand programme ends: if the channel is now airing something
  else, the channel rejoins live; if the channel is now airing the very
  programme just watched, it starts the next programme from its beginning
  instead — **never replay what was just watched**. Changing channel,
  a game, the Media Browser or a restart all return to live. **(owner:
  snap back to live; Claude's call: the never-replay rule)**

## Constraints

- Dual-board contract (CLAUDE.md): runtime-only board differences via
  `PlatformProfile`; Pi 4B performance envelope; both boards from one
  golden image and one OTA.
- Media Browser ON and OFF builds: movie channels are compiled out with
  the Media Browser (`MEDIA_BROWSER_ENABLED`); daily lineups and
  pick-ahead exist in both.
- No file I/O or network on the render loop; Radarr calls and snapshot
  writes on workers; decisions in pure, Mac-tested units; kiosk-only
  files only gather facts and paint.
- Golden image and OTA rules (the snapshot is private; new data files get
  the excludes `media_info.json` has).
- Family-safe filter (CLAUDE.md) stays in force; the Kids rule above is
  stricter on top of it.

## Approaches considered

1. **The kiosk builds movie channels itself (chosen, Claude's call).** It
   already holds the Radarr client, the unlock flag and tunnel health;
   channels stay one system in C++ with pure tested units. Cost: a
   background refresh worker and a snapshot file in the kiosk.
2. The web admin writes generated "movie playlists". Reuses the playlist
   path, but they would appear in List style and the Content Manager, and
   golden images would carry the owner's titles as playlists.
3. The web admin snapshots Radarr and the kiosk reads it. Two programs
   share one job; no gain over 1.

## Left to the builder

- Snapshot format and refresh back-off; how the refresh learns of a
  finished download sooner (optional).
- Panel and cell wording for movies (year placement), CRT layout of
  pick-ahead, how far the time window scrolls per turn.
- How the movie key enters the per-video store, provided the default is
  "whole picture" and the Content Manager is unaffected.
- Test fixtures (a fake Radarr list with genres, certifications, codecs).
