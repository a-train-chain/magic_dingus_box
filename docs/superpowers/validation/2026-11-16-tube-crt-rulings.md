# Tube CRT engine — decisions made without asking

While the Tube engine was built (plans 1–3 and the switch-on, 2026-11-15/16) the work did not stop to ask about
every question. Each decision below was made on the spot and recorded with its reason and what it costs if it
was wrong. Decisions the owner made himself (all 44 rows editable on TV and web, both frame rates, drop
afterglow, films fall back to Enhanced, finish before deploying) are not listed here.

## 1. Decisions that change what you see

| Decision | Why | Cost if wrong |
|---|---|---|
| All six looks run at 30 fps; none was retuned to reach 60 | You asked for Sound Factory's values unchanged; the trial "PVM lean" look missed its own smoothness rule | The default look is 30 fps where a retuned one could have been 60 |
| Noise, hum, flicker, rolling band, purity, ABL and breathing force 30 fps | Measured as groups that could not hold 60 fps | Someone using only hum or flicker gets 30 fps where 60 may have been possible |
| The "This CRT look is too heavy" notice needs three whole minutes in a row with more than half the frames late, or ten in a row above 5 %; a frame gap over 100 ms counts as the box stalling and restarts the count | The first rule (5 % over 30 s) and the second (half over 30 s) both fired against shipped looks while the box was briefly busy — three times on 2026-11-16, once on the lightest look | A look that really is too heavy is reported after three minutes, not 30 seconds |
| A look never loses glow or afterglow unless that actually gets it under budget | Losing effects for the same 20 fps helps nobody | A look that is over budget keeps its effects and runs late |
| Only a real full-screen film is handed to Enhanced, not any large picture | The first version also handed the menus to Enhanced in one transient state | None found |
| When Tube cannot start, the screen says so in plain words: "Tube could not start on this box, so it is showing the Enhanced look. Restart the box to try again." / "Tube could not draw this look… Try a different look." | The first wording was internal jargon | Wording only |
| No "CRT Look" web tab at all on a box that is not offered Tube (Pi 4B) | The design said two different things; the acceptance list said "no tab" | A one-line front-end change |
| After a game the picture comes straight back, with no warm-up animation | A warm-up after every game would be worse than Enhanced | One extra flag in the renderer |
| In CRT Native the look picker does not show a frame rate | The rate shown there was misleading | Wording only |
| Two speed-ups were shipped only because they do not change the picture (values worked out on the CPU; the wide glow blurred at half size) — at most 1 step in 255 on the shipped looks | They bring every look to 30 fps at 1080p | A barely measurable change in the halo |
| The cheap beam was NOT put on the rich looks, because it changes the picture (up to 9–33 steps in 255) | "Faithful port" was the brief | The heavy looks have less headroom |
| A rewritten output pass was NOT shipped: it drew the same picture but measured about 6 % slower | My first explanation of the slowness was wrong, and the measurement showed it | None |

## 2. Decisions about how it is built

| Decision | Why | Cost if wrong |
|---|---|---|
| Live tuning from the web uses a numbered list of changes, not a copy of the whole setting | Two phones can no longer undo each other's changes | A change made in the instant before a kiosk restart is not applied (the page then shows the real value) |
| The engine stayed hidden behind one switch until the renderer was finished | A release could be cut from main at any time | You could not preview the controls on a box without a test build |
| A box that is not offered Tube constructs and runs none of it | The Pi 4B must be untouched | None found |
| The build guard recognises its marker file by content, not by file identity | Linux reuses file identities, which made the guard delete the wrong file in CI | None found |
| The developer validation switch (`MDB_TUBE_VALIDATE`) stays in the normal build; the test-only preview build and the deliberate-hang hook were removed | Any change to the shader table needs a hardware validation run; a deliberate hang must not ship | One environment read at start |
| Several small review findings were fixed at once instead of deferred (a blur that could produce invalid numbers, a missing shader-value list entry, the degauss strength cap, a malformed mask, per-frame allocations, and others) | Each sat in a file already open and could have become a wrong picture later | Slightly larger changes to review |
| Ported behaviour the plan's text had left out (a tracking slip settles over a few frames) | The design says "faithful port" | A few extra frames drawn after a slip |
| The plan's precision for one test constant was wrong; the test uses the exact formula | The literal did not pass at the default tolerance | None |

## 3. Decisions about the process

| Decision | Why | Cost if wrong |
|---|---|---|
| Reviews ran on Opus 5.5 / Sonnet, not the most capable model | You asked to keep Fable for the hardest tasks | A subtler defect missed |
| The first branch was rebased onto GitHub's main (local main was 238 commits behind) | A PR against a stale base would not merge | A backup branch `tube-crt-core-pre-rebase` exists |
| The whole-branch fix wave ran before hardware testing, not after | Hardware should test what will ship | Later changes got scoped reviews instead of one big one |
| The switch-on was prepared as a draft PR before the last hardware checks | The checks should run the exact binary that would ship | A draft that needed changes afterwards (it did: the notice rule) |
| One worker was allowed onto the Pi for an off-screen shader tool | It could not disturb the TV; the other session agreed | Noise on the shared box |
| No test kiosk on your TV without your say, and never with a time limit | A film you had resumed on a test kiosk was interrupted on 2026-11-15 | — |
| A change that stopped the notice judging frames under a menu was written and then backed out, unpushed | Clean re-measurements showed menus were not the cause | None; the branches stayed as reviewed |
| "Continue on with your recommendations" was read as: merge #33 then #34 in order and leave other refresh rates test-only — not as leave to run the on-box update rehearsal or to tag | Those two change your box or publish something | One more question to you |
| #34 was merged after a joint hardware check of all three changes in one binary, pinned to the tested commit | All three change the kiosk's main loop | A day's delay at most |

## 4. Left as they are (known, small)

- A degauss pressed from the web during a game fires when the game ends.
- After a failed build the engine keeps about 32 MB of render targets until the Media Browser opens, the engine
  changes, a game starts or the picture is resized.
- CRT Native keeps the signal rows (chroma bleed, convergence) and drops the rows a real tube draws itself; on a
  flat panel in CRT Native that reads as colour fringing with no scanlines. This is the design's call.
- The rule for the notice needs unbroken minutes of video; a box that stalls for a tenth of a second inside
  that time starts the count again, so a box that stalls every minute never shows the notice.
- Small test gaps noted by reviewers: no test at exactly 5 % late in a minute; two stale comments in
  `tube_pacing.h`.
