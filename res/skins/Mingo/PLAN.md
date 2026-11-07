# Mingo implementation plan

Working from: Tango baseline (commit 4e9f97c6d0). See DESIGN.md for the
goal statement. This is the file-by-file plan; nothing here is applied
until reviewed.

## Current Tango structure (what we start from)

Vertical stack in `skin.xml`:

```text
topbar.xml                       logo, main/headmixer toggles, VU, gear
WidgetStack (show_maximized_library)
 ├─ Splitter
 │   ├─ waveforms_container.xml  4 stacked scrolling waveforms (~22px each)
 │   └─ decks_12.xml             decks 1+2 side by side
 │      fx_units_34.xml          (hidden unless show_4effectunits)
 │      decks_34.xml             decks 3+4 side by side (show_4decks)
 │      fx_units_12.xml          FX units 1&2, one row
 │      sampler_rack.xml, mic_aux_rack.xml
 │      library.xml              tracks table + crate sidebar
 └─ maximized-library mode       2 mini-deck rows + full library
skin_settings.xml                slide-out gear menu
```

Each deck (`decks/deck_left.xml`) is: overview + comment + artist/time
row, transport buttons row, loop/beatjump row, spinny/cover, pitch
slider + key controls, VU meter. Scrolling waveform height is only 22px;
beatgrid buttons toggle to 50px.

Deck pairing is an engine feature on this branch (`deckpairtransport`) —
the skin does not need to implement pairing, it only needs to *present*
two logical pairs (1+3, 2+4) instead of four decks.

## Decisions from review round 1

- D1: Native 1280x720, hand-sized widgets (48-64px touch targets),
  no Mixxx --scale.
- D2: NO load controls on screen. The touchscreen selects tracks;
  loading is the SC Live 4's dedicated load button per deck.
- D3: Library is a permanent bottom pane (~200px).
- D4: FX Unit 1 is the only unit shown — its active chain preset name
  lives in the topbar, display-only. No FX information anywhere else
  (strip dropped from the layout).
- D5: Keep the Mixxx logo in the topbar.
- D6: Stretch goal (Phase 6): per-pair split waveform — top half =
  lower-numbered deck, bottom half = higher-numbered deck.

## Target layout (regular mode, 1280x720)

```text
[topbar: Mixxx logo | FX1 active preset name | lib-maximise toggle | gear]
[left pair column (decks 1/3)   | right pair column (decks 2/4)]
   title (large) + key              title (large) + key
   artist                           artist
   WF 1 zoomed (h~70)               WF 2 zoomed
   WF 3 zoomed (h~70)               WF 4 zoomed
   overview 1 (h~34)                overview 2
   overview 3 (h~34)                overview 4
[library pane: crates sidebar + tracks table, ~200px]
```

(Phase 6, if it lands, merges each pair's two zoomed WF rows and two
overviews into one split row per pair, freeing height for zoom.)

Waveform zoomed rows stay per physical deck so slip divergence is
visible; in normal paired operation the two rows of a pair mirror each
other. Colour-code by pair (Tango already has SignalColor_12/_34 — we
would re-purpose to pair colours: 1+3 vs 2+4).

Vertical budget (must fit 720 exactly): 28 topbar + 2×24 text rows
(title/artist shared visually per column) + 4×70 WF + 4×34 ovr +
200 library = 688, leaving ~24px slack for splitters/margins.
Zoomed WF height (66-76) is the flex dial; library pane is the second.

Maximised-library mode: keep the concept — full-screen library + two
slim pair rows (per-pair title/artist/WF thumbnail) for load previews.

## Phase plan

### Phase 1 — Strip (mostly attribute defaults, low risk)

- `skin.xml` manifest: `show_coverart=0`, `show_spinnies=0`,
  `show_big_spinny_coverart=0`, `show_rate_controls=0`,
  `show_starrating=0`, `show_loop_beatjump_controls=0`, `vumeter=0`,
  `show_stem_controls=0`, `trackComment=0`,
  `show_4decks=1` (waveform rows 3/4 are bound to this).
- `skin.xml` main layout: drop `sampler_rack.xml`, `mic_aux_rack.xml`,
  `fx_units_34.xml` references; keep the templates on disk for now.

### Phase 2 — Two-column paired layout (the big one)

- New `pair_column_left.xml` / `pair_column_right.xml` replacing
  `decks_12.xml`/`decks_34.xml` in `skin.xml`.
- New `decks/deck_text_big.xml`: title (large), artist, `initial_key`
  TextProperty; NO NumberPos time, NO duration (replaces
  `row_text_*.xml` in our layout).
- `waveform.xml`: new slim variant — Visual only (beatgrid buttons stay
  optional), height grown to ~70px min; remove stem_control block.
- `decks/overview.xml`: reuse as-is, height ~36, tap-to-scan works
  natively (OverviewWidget seeks on click).
- Remove per-deck rows from our layout: `row_transport_*`,
  `row_loop_jump`, `rate_pitch_key`, `spinny_cover_*`, `vumeter_*`,
  hotcue rows. Files remain for the settings menu to re-enable if we
  later want an "extended" mode.

### Phase 3 — FX name in topbar (display-only)

- Topbar gains the FX1 active chain-preset name next to the logo:
  `EffectChainPresetButton` for `EffectRack1_EffectUnit1`, restyled in
  style.qss as a wide text button (name centred, ~140-180px wide,
  elided); tapping opens the chain preset chooser, which we accept.
  Fallback if text rendering misbehaves: `EffectPresetNameLabel`-style
  read-only TextProperty beside a hidden button.
- No other FX UI anywhere: remove `fx_units_12.xml` reference and all
  `fx/` templates from our layouts; units 2-4 never shown. The
  manifest's routing-disabling attributes for them stay as-is.

### Phase 4 — Library for touch

- `library.xml`: hide preview deck, hide cover art pane; keep crate
  sidebar + tracks table splitter.
- No load/eject widgets — the touchscreen only changes the library
  selection (standard table tap); hardware loads it.
- Column set: bump table min row height in `style.qss` for finger
  targets (~44-56px rows at native 1280x720); preferred columns incl.
  title/artist/key via [Library] config defaults in the manifest.

### Phase 5 — Touch ergonomics & chrome

- Topbar: keep logo; remove mixer/PFL/BPM readouts & toggles; keep
  FX1 preset name (Phase 3) + maximise toggle + gear.
- Audit every remaining tappable widget ≥ ~48px (≈ 6 mm at 210 ppi);
  bump in style.qss/templates.
- `MinimumSize` stays ≤ 1280x720 with no scrollbars at that exact size;
  verify with `mixxx --mixxxcfg /tmp/cfg --resourcePath .`.
- Replace `skin_preview.png`; prune skin_settings.xml entries for
  removed widgets (dead toggles confuse on the device).

### Phase 6 — Split pair waveforms (hard; C++ likely required)

Goal: one waveform row per pair — top half of the display is the
lower-numbered deck (1 in pair 1/3, 2 in pair 2/4), bottom half is the
higher-numbered deck. With a healthy pair the two halves overlay into
one waveform; during slip they visibly peel apart. One row per pair
instead of two would also buy back ~140px of height for zoom.

Why it is hard: the skin `Visual` widget binds exactly one
`[ChannelN]`, and the waveform renderer draws each deck's envelope
symmetrically about that widget's centre line — there is no clipping,
masking, or per-half deck binding in skin XML.

Options, in evaluation order:

- 6a. Skin-only prototype (zero C++, quick): stack two `Visual`
  widgets and try to mask the loser halves with opaque overlays.
  Expected outcome: fails — a half-height `Visual` re-renders a
  squashed full waveform (envelope scales with widget height) rather
  than clipping, so this only tests whether the overlap looks right.
  Timebox: half a day; do it only to inform 6b.
- 6b. C++: extend the waveform widget (recommended).
  - Skin XML: new optional `<PairBottomChannel>` (or
    `SplitPair="true"`) on `Visual` in `src/widget/wvisual.*`.
  - Renderer: when set, draw `Channel`'s waveform clipped to the
    upper half (zero line at widget centre, amplitude budget
    height/2) and the bound second channel clipped to the lower
    half; one shared playhead; cue/loop marks drawn on their own
    deck's half; loop ranges from the pair leader.
  - Zoom/scroll: both halves ride the pair's shared transport, and
    this fork's `deckpairtransport` already defines the pair plane
    both decks track — positions are directly comparable without new
    engine plumbing.
  - Touch-scan overviews get the same split treatment (second widget
    type, same masking idea).
  - Risk: waveform render internals (per-deck render pass assumes it
    owns the full widget height); keep it opt-in per widget so Tango
    and every other skin are untouched.
- 6c. QML skin engine alternative (2.6 QML skins support stacked /
  mirrored pair waveforms out of the box): rejected — would mean
  rebuilding the entire skin in a different engine for one feature.

Milestones: 6a visual mock in the layout (0.5d) → decision → 6b
prototype on pair 1/3 (2-3d) → pair 2/4 + overviews (1d).
Fallback if 6b stalls: two full waveforms per pair, tightly stacked
(what Phases 1-5 already give us).

## Assumptions (say otherwise)

- A1: Samplers, mic/aux, headphones/PFL: not shown at all. The gear
  menu keeps a Samplers toggle so they're recoverable, but off by
  default.
- A2: No transport (play/cue/sync) on screen at all — pure display +
  library + FX-slot toggles. Deck load (Q2) is the only deck action.
- A3: Track comment row (Tango feature) removed.
- A4: Loop indicator / hotcue states not shown (they're on the pads).
- A5: The gear skin-settings menu itself stays (slimmed), so settings
  remain reachable on the device without a keyboard.
- A6: No eject on screen either — deck operations live on the
  hardware (D2 implies this); revisit on the device if a wrong load
  ever needs a keyboard to fix.

## Resolved questions (round 1)

- Q1 → D1: native resolution, hand-sized widgets.
- Q2 → D2: no loading on screen; hardware load button.
- Q3 → D3: permanent bottom library pane.
- Q4 → D4: FX Unit 1 only, display-only strip.
- Q5 (comment row) → dropped per A3 unless revived later.

---

## Build log

- **Phases 1–5 complete.** Headless verification recipe (no display on
  the build box): seed `/tmp/mcfg` with `[Config] ResizableSkin Mingo`
  plus a `directories` row in `mixxxdb.sqlite`, then
  `QT_QPA_PLATFORM=offscreen ./build/mixxx --resource-path res
  --settings-path /tmp/mcfg --safe-mode --developer` and grep the log
  for `Skin parsing failed` / `Invalid` — must be empty. (The old
  `--mixxxcfg` flag above predates 2.6; it is `--settings-path` now.)
- **Phase 2 fixup:** the deck-header key display used `<TextProperty>`,
  which the legacy parser silently skips; replaced with the dedicated
  `<Key>` widget. Skin warnings must be grepped from every smoke run.
- **Phase 5 outcomes:** dead singleton definitions (VU meters, FX-unit
  stack, sampler-row settings) and ~40 dead manifest defaults removed;
  skin settings rewritten as a slim Mingo page (Display / Mini Decks /
  Branding); topbar chrome (FX preset selector, gear, rec, broadcast)
  and the library maximise toggle enlarged. The Tango-era template
  files that nothing references any more (old deck columns, mixer,
  samplers, size detectors) are kept in-tree for now — no dynamic
  `src` references exist, so deleting them later is safe but unneeded.
- **Deferred:** `skin_preview.png` still shows the Tango layout (no
  way to screenshot the Mixxx GUI headless on this box); Phase 6 split
  pair waveforms unchanged.
