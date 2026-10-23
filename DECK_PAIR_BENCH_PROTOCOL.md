# Deck Pairs — Bench Test Protocol (Phase 5 sign-off)

> **AI-generated document.** This text was generated autonomously by an AI
> coding assistant while implementing Phase 5 of `DECK_PAIR_PLAN.md`. It has
> not been executed by a human: the whole point of this document is that a
> person runs it on real DJ gear. Every "observed" box below is deliberately
> empty. Treat any statement in here about *behaviour on hardware* as
> unverified until those boxes are filled in by the developer.

## Why this document exists

`AGENTS.md` requires that a change is tested by a human on a real DJ setup
before it can become a pull request, and that AI agents never open PRs. Phase 5
of the deck-pairs plan cannot be signed off by the assistant that wrote it: the
automated suite (T1–T38, 90 tests in `mixxx-test`) covers engine and manager
invariants, but it cannot tell you what the pair does to a vinyl crate, a
controller with a torque motor, or a room.

**Sign-off is therefore pending on you.** Fill in the date, the gear, and the
observed result of each numbered step. Anything that fails: leave the step
marked FAIL, note what happened, and do not open the PR.

## How to record

For each step write `PASS`, `FAIL` or `N/A` plus one line of what you actually
saw (times, artefacts, controller model). Audio artefacts are hard to describe
after the fact — record them with `Capture to File` (deck cue -> right-click ->
Record) or an external recorder on the master out, and name the file in the
notes column. Keep the notes; they become the PR description.

## Gear under test

| Item | Value |
| --- | --- |
| Date | |
| Mixxx version (`--version` output) | |
| Git commit / `git status` clean? | |
| Audio device + buffer size | |
| Sample rate | |
| Controller(s) / timecode setup | |
| Skin | |
| Tracks used (note BPM, length, stems or not) | |

## Baseline before you start

0.1  Build with STEM enabled, run the automated suite first and confirm it is
  green, so a bench failure below is a bench finding and not a build issue:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTEM=ON
cmake --build build --target mixxx-test -j"$(nproc)"
cd build && ctest -R "DeckPair" --output-on-failure
```

Result: ______________________

0.2  Start Mixxx with two decks loaded with different tracks. Confirm decks 1
  and 3, and 2 and 4, are the pairable combinations, and that `pair_enabled` is
  visible in the right-click menu of a deck transport (**Transport -> Deck
  Pair**). This is the ControlPickerMenu entry added in Phase 5 and is also what
  you map to a controller button.

Result: ______________________

---

## A. The core illusion — one deck, two channels

The claim: a locked pair behaves like *one deck whose audio appears on two
channel strips*, while each member keeps its own mixer strip and its own
stems.

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| A1 | Pair 1+3 (`pair_enabled` on deck 1). Deck 3 adopts deck 1's track and position. | One track, both strips; no second load indicator stuck on | | |
| A2 | Press play on **either** member. | Both start together, in phase; a single transport state | | |
| A3 | Nudge / scratch on the follower. | The pair moves as one; no echo, no flam, no doubling | | |
| A4 | Tempo adjust on the follower, then on the leader. | Same audible pitch change; both rate indicators agree | | |
| A5 | Keylock on one member only. | Deck-local; the pair stays in tempo (key may differ between strips) | | |
| A6 | Volume, EQ, filter, and FX unit on each member independently. | **Independent**: the pair is not a copy of one strip (R9) | | |
| A7 | Cue / cue point set on the follower. | Cue point lands on the shared track; cue preview works on the deck you pressed | | |

## B. Transport operations

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| B1 | Loop in/out from the follower, with quantize ON. | Loop engages once, on both members, on the beat, no double-trigger | | |
| B2 | Beatloop 1/2/4/8 from either member, then release. | One loop region; release resumes at the loop's exit point | | |
| B3 | Loop halve/double while audible. | Region changes on both; no click at the change | | |
| B4 | Seek by waveform on each member. | Both jump to the same position; expect a short (a few ms) silent re-anchor on the follower, not a repeated click | | |
| B5 | Jump to cue, and to previous/next cue point. | Both members move together | | |
| B6 | Track end: let the pair run to the end of the track. | Both stop; no restart loop; nothing on the follower's reader hammering the CPU (watch the CPU meter) | | |
| B7 | Load a *different* track onto the follower while paired. | Refused or routed to the leader per plan (R3), never a divergent pair | | |
| B8 | Eject from the follower. | Both eject (or the "load when deck playing" safeguard refuses while audible). Check your `[Library]LoadWhenDeckPlaying` setting behaves as it does for a normal deck | | |

## C. Slip, rolls, and the deck-local exceptions (R8)

This is the part most likely to surprise a DJ, and the part the automated
suite can only assert arithmetically.

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| C1 | Slip mode on the leader, then release. | Only the leader slips; the partner keeps playing. On release the leader joins the pair position again without a jump | | |
| C2 | Slip mode on the follower, then release. | Same, mirrored | | |
| C3 | Hold a hotcue (hotcue roll) on one member. | **Deck-local roll**: that deck rolls, the partner keeps playing. Release resumes the pair | | |
| C4 | Beatloop roll (tempo reset / slip roll gesture) on one member. | Deck-local, as C3 | | |
| C5 | Reverse roll on one member. | Deck-local | | |
| C6 | Engage C1 while the other member is mid-roll. | Both independent; no stuck positions afterwards | | |
| C7 | Slip release exactly on a loop boundary. | No audible snap, no double-length loop | | |

> Mapping note for C3–C5: a hotcue, beatloop-roll or reverse-roll button keeps
> its ordinary per-deck meaning on a paired deck. A mapping does **not** need
> to change, and a mapping that tries to mirror these gestures to the partner
> is fighting the design (R8).

## D. Stems (if you have stem files)

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| D1 | Pair two decks on a stem track. Mute the vocal on deck 1 only. | Partner keeps the vocal: two data planes (R9/R11) | | |
| D2 | Solo an instrument on the follower, stem gain on the leader. | Independent, and the transport stays locked | | |
| D3 | Stem quick-effect on one member. | Independent; transport unaffected | | |
| D4 | Split stems across the pair (e.g. instrument out of 1, vocal out of 3), play a whole track. | Stays in phase for the whole track; no gradual flam | | |

## E. Interlocks — passthrough, vinyl, AutoDJ

The dangerous cases are the leader with vinyl input, and AutoDJ refusing to
start because the pair mirrors its own play state.

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| E1 | With a pair engaged, enable passthrough on the **follower**. | Passthrough stays on, pair **disbands**. No stuck audio | | |
| E2 | Same on the **leader**. | Same: disband, not a degraded pair. This is the case that used to leave the follower replaying a muted transport | | |
| E3 | With timecode enabled, move the vinyl on one member. | Behaves as passthrough per your preferences; never a pair that plays two tracks | | |
| E4 | Pair 1+3 playing, decks 1+2 bound to AutoDJ. Press **Enable Auto DJ**. | **Starts.** (Phase 5 fix: the mirrored partner is no longer counted as an "unused deck playing".) Before the fix this flashed an unusable error | | |
| E5 | Deck 3 playing, *unpaired*, decks 1+2 stopped. Press Enable Auto DJ. | Still refused (`ADJ_UNUSED_DECK_PLAYING`) — a real third deck must keep AutoDJ off | | |
| E6 | AutoDJ running, then pair the deck AutoDJ is fading into. | No double track-queue advance, no track skipped, no stuck fade | | |
| E7 | AutoDJ transitions with a pair on the left deck. | The pair fades as one deck; the follower's strip does not stay loud | | |

## F. Load-refusal behaviour changed in Phase 5 (please confirm it feels right)

Phase 5 changed one thing a DJ can observe: if propagating a load onto the
partner is **refused** by the *Load when deck playing* preference, the pair is
now **disbanded** instead of left playing two different tracks on one
transport.

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| F1 | Set *Load when deck playing* = **Reject**. Pair 1+3. Make the follower play alone (slip + play). Load a new track on the leader. | Leader gets the new track, partner keeps playing its own, and the **pair is released**. You hear two decks, not garbage | | |
| F2 | Set it to **Allow**. Repeat. | Partner loads the new track and keeps playing it | | |
| F3 | Set it to **Allow but stop**. Repeat. | Partner loads and stops | | |
| F4 | Now deliberately try to re-pair straight after F1 while both are still playing. | **Refused** (pairing onto an audible deck). You must stop one deck first. Confirm this reads sensibly rather than as a bug — if it does not, say so, it is a design decision worth revisiting | | |

## G. Endurance and load

The automated soak is 2000 callbacks of one locked pair. A gig is hours, on a
laptop with other things running.

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| G1 | Run a pair for 30+ minutes of continuous playback with rate changes and loops. | No gradual drift (use `Decks -> Sync Lock` off; watch the waveform overlay), no creeping CPU | | |
| G2 | Watch the engine XRUN / overruns counter (`[Mixxx]Beat`… or the engine log) before and after. | No new overruns attributable to pairing | | |
| G3 | Deliberately stress: 4 decks, two pairs, stems on both, small buffer (e.g. 64 frames), weak machine if you have one. | Degrades as two independent decks do — that is the intended price of two data planes; it must not degrade *further* or desync | | |
| G4 | Hot-swap audio device / change buffer size while paired. | No crash; either the pair survives or it releases cleanly | | |
| G5 | Quit Mixxx while paired. | Clean shutdown, no crash on exit, no stuck pair on next start (pairs are runtime-only, nothing persisted) | | |

## H. Controller mapping

| # | Step | Expected | Pass/Fail | Notes |
| --- | --- | --- | --- | --- |
| H1 | Map a button to `[ChannelN], pair_enabled` and toggle it from the controller. | Toggles reliably from the controller thread. Phase 5 made the manager's listeners thread-safe; a hang, crash, or "worked once then stopped" here is the finding this step is looking for | | |
| H2 | Toggle pair from the controller while the other member is playing. | Refused with a log line, button state returns to off | | |
| H3 | Twist the pair's tempo fader while it is playing. | Same as an unpaired deck; no stepping or lag | | |
| H4 | If your controller sends `passthrough` (vinyl button): press it while paired. | Case E1/E2, from hardware | | |

## I. What the automated suite could not cover

Stated so the bench does not duplicate effort, and so a PR reviewer knows what
is and is not proven:

* `EngineBuffer`'s **stall check** (a leader that stops publishing while the
  follower stays locked): correct by construction and covered for the recovery
  path by T34's pause/resume, but the fault itself — leader wedged, follower
  still locked — is not injectable from a unit test. G1/G3 are the proxy. If you
  ever see a paired follower *repeatedly* click or stutter at a fixed position,
  that is this code path; note the moment and the log.
* Real-time behaviour under a real scheduler (T43 runs a 3-thread model, not a
  real audio device).
* Timecode/vinyl tracking on real vinyl (E3).
* Anything involving speakers: clicks, pops, latency between the two strips.

## Least-recently-tested, most-likely-wrong

If you only have twenty minutes, do **E1, E2, E4, F1, C3, H1, G5**.

---

## Sign-off

* [ ] All steps above recorded, with failures listed below.
* [ ] Failures either fixed or explicitly accepted, with a note.
* [ ] `ctest -R "DeckPair"` green on this machine at this commit.
* [ ] Sanitizer run attached (see `DECK_PAIR_PLAN.md` 9.18, "Sanitizers").
* [ ] Developer name: ______________________   Date: ______________

> **AI-generated document.** The above was written autonomously by an AI coding
> assistant as part of Phase 5 of `DECK_PAIR_PLAN.md` and has not been executed
> on hardware. No claim in it about runtime behaviour on a DJ rig is verified
> until a human fills in the results.
