# Mingo skin — design decisions

Mingo is a fork of the Tango skin, redesigned as a touchscreen companion
display for a Denon SC Live 4 standalone build.

## Deployment target

- 7-inch-class display, 1280x720, approx. 210 ppi, capacitive touch.
- Built into a Denon SC Live 4 chassis (standalone DJ system).

## Operating model

- The skin does NOT duplicate information or controls available on the
  SC Live 4 control surface. The hardware handles performance controls;
  the screen handles what the hardware cannot show.
- Paired-decks mode is assumed at all times: decks 1+3 and 2+4 operate
  as hard-synced units unless slip is enabled on either deck. Layout
  should therefore read as two logical decks, not four.

## Information hierarchy

Critical (must always be visible):

- Waveforms, zoomed — what is playing right now
- Waveforms, per deck — for scanning through tracks
- Artist name
- Song (title) name
- Current FX preset name

Good to surface (visible when space allows, can be secondary):

- Library (crate / track browsing)
- Track key

Not needed (remove or hide):

- BPM
- Time elapsed / time remaining
- Any control-position readouts (knob/fader values)
- Track artwork
- Spinning disc (spinny) animations

## History

- Forked from Tango at Mixxx commit 4e9f97c6d0 (baseline commit).
