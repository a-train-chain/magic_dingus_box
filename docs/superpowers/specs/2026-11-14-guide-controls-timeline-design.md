# Guide controls and timeline refinement

Status: approved by the owner on 2026-11-14, with authorization to write the plan
and carry implementation through review/testing without another approval stop.
Follows the completed Movie Channels work.

## Intent

Watching a highlighted channel should take one press. Playlist videos must stay
readable on the shared timeline, even while browsing feature-length movies.
The overnight movie-channel gap must be explicit instead of looking like missing
schedule data.

## Approaches and owner decisions

- Owner chose **tap to watch; hold to enter Pick Ahead**, over hold-and-turn.
  Separate short/long presses avoid requiring a turn while the knob is held down.
- Owner chose **keep the live 1 AM cutoff**, with a labeled off-air gap. Manually
  selected programmes continue to finish whole, including after 1 AM.
- Owner rejected fitting an entire movie by zooming out: it makes playlist clips
  too small. Keep a short shared timeline and allow long films to be clipped.

## Interaction

- The hold-to-browse gesture applies in Guide Mode: Pick Ahead. Live mode retains
  its existing one-press watch behavior.
- In the open Channel Guide, one short Select/encoder press watches the highlighted
  channel immediately. While browsing future programmes, it plays the selected
  programme from its beginning as today.
- Holding Select for **500 ms** enters Pick Ahead on the highlighted channel;
  turning then steps programmes. Releasing that hold must not also play anything.
  The threshold is an implementation proposal, chosen to distinguish an ordinary tap.
- From fullscreen playback, a short press opens the guide. A hold opens the guide
  and enters Pick Ahead on the tuned channel when it is eligible.
- Short Back leaves programme selection for channel rows. Phone/pad left/right
  still enter and navigate programme selection directly. Black/menu hold-volume
  remains independent. Menus, games and List style keep their existing Select behavior.
- Hints describe the active actions: press to watch, hold to pick ahead; once
  browsing, turn for programmes and Back for channels. Cancel pending holds on
  context changes so a delayed release cannot trigger another screen's action.

## Shared timeline

- Keep the existing **20-minute Modern TV / 15-minute CRT** window in every guide
  state, including future selection. All rows use the same time axis.
- Browse ahead by moving that fixed-size window to the selected programme's start;
  never widen it to fit a film. Preserve the existing five-minute tick cadence.
- A movie extending beyond either visible edge gets a continuation marker at that
  edge. The visible portion remains selected/highlighted and its title remains readable.
  The upper panel keeps the scheduled start/end and next-programme information.
- When a movie channel closes at 1 AM within the visible window, fill the remaining
  visible interval with **OFF AIR · BACK AT 6:00 AM**. Fit the copy to the available
  width without reducing the established text-size floor; narrow portions may use
  **OFF AIR**, while the panel supplies the return time. This interval is not tunable.
- For the last movie before closing, the panel identifies the next scheduled film
  at 6 AM. Once off air, retain the existing off-air row presentation. Playback
  deadlines and the daily lineup remain governed by the existing schedule.

## Implementation and acceptance boundaries

Use pure input/view logic with thin kiosk wiring, existing guide typography and
colors, and no new I/O on the render loop. Both Media Browser builds and both board
profiles must remain supported. Tests cover short/long press and cancellation,
fixed-width future navigation, clipping/continuation, the overnight gap and next
morning lookup. Verify the real encoder/phone on the Pi after reviewed deployment.

Continue the paused physical SSD test afterward, then delete the three explicitly
approved test files and restore only test-owned settings/service changes. Preserve
Bluetooth during local integration, run deployment dry runs, use one Mac scratch
build and remove it afterward. No push or shared-branch merge is authorized.
