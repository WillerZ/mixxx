// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#include "mixer/deckpairmanager.h"

#include <QThread>
#include <QTimer>
#include <cstring>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "engine/sync/syncable.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "moc_deckpairmanager.cpp"
// Backport note (2.6): LoadWhenDeckPlaying lives in preferences/dialog/dlgprefdeck.h
// on this branch (2.7 moved it to preferences/interface.h).
#include "preferences/dialog/dlgprefdeck.h"
#include "util/defs.h"
#include "util/logger.h"
#include "util/math.h"

namespace {
const mixxx::Logger kLoggerDeckPair("DeckPair");

constexpr qsizetype kPairSlotDistance = 2;
constexpr qsizetype kNumberOfPairSlots = 2;

constexpr char kPairEnabledItem[] = "pair_enabled";

/// IEEE bit cast for the deferred relay carriers' atomics: QAtomicInteger
/// has no floating-point specialization, so the doubles cross threads as
/// qint64 bits (9.19).
qint64 doubleToBits(double value) {
    static_assert(sizeof(double) == sizeof(qint64));
    qint64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
double bitsToDouble(qint64 bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Controls which would make a deck a transport authority of its own. On a
/// pair follower they are refused, because the pair leader alone computes the
/// transport. All of them are owned by engine controls which confirm writes
/// themselves (`ControlObject::set()` alone does not change their value), so a
/// refusal both undoes the pending engine request and snaps the control back
/// to 0. Slip controls are deliberately *not* part of this list: slip stays
/// strictly deck-specific in every situation (R8).
const QStringList kRefusedFollowerAuthorityItems = {
        QStringLiteral("sync_enabled"),
        QStringLiteral("sync_mode"),
        QStringLiteral("sync_leader"),
        QStringLiteral("vinylcontrol_enabled"),
};

/// Controls which take the requested value immediately (no confirmation by an
/// owner). They are watched for a value change and reset instead.
const QStringList kRefusedFollowerPlainItems = {
        QStringLiteral("vinylcontrol_wantenabled"),
};

/// The controls whose engine side needs an explicit undo request.
bool isSyncAuthorityItem(const QString& item) {
    return item.startsWith(QLatin1String("sync_"));
}

/// Command keys relayed from the pair follower to the pair leader
/// (DECK_PAIR_PLAN.md 5.5, the full Phase 3 table). Slip keys must never
/// appear in this table in either direction (R8; T30 asserts it statically):
/// a slip gesture stays on the deck that made it, and the bridge is fully
/// disconnected while the follower slips (9.11 m) -- the mirror side is also
/// halted while the *leader* slips (9.16).
///
/// Documented deviations from the plan's key list:
///  - This tree calls the rate-range control `rateRange` (upstream
///    `rate_range`), names the cue hold-and-play control `cue_play`
///    (`cue_cue_play` does not exist here), and suffixes the loop-move
///    family by size (`loop_move_1_forward`, not `_1_beat_forward`).
///  - The shared-Track beatgrid family (`bpm`, `beats_adjust_*`,
///    `beats_translate_*`) is *not* relayed although 5.5 listed it: these
///    controls mutate the single Track both members share, and the acting
///    deck's own BpmControl already applies the edit exactly once -- both
///    engines then adopt the new grid through the Track's beats-changed
///    signal. A relay would apply the same edit a second time on the
///    leader. The *tempo* result of a follower BPM edit still moves the
///    pair through the `rate_ratio` relay (the universal rate net, 9.11 d).
const QStringList& fullRelayItems() {
    static const QStringList items = []() {
        QStringList relay = {
                // Transport and play state.
                QStringLiteral("play"),
                QStringLiteral("start_play"),
                QStringLiteral("start_stop"),
                QStringLiteral("stop"),
                QStringLiteral("start"),
                QStringLiteral("end"),
                QStringLiteral("playposition"),
                QStringLiteral("repeat"),
                QStringLiteral("quantize"),
                // Cue family, press+release pairs included. The follower's
                // preview machinery is gated while locked (cuecontrol.cpp),
                // so the command reaches the leader through the bridge and
                // the follower rides the pair plane (T18).
                QStringLiteral("cue_default"),
                QStringLiteral("cue_set"),
                QStringLiteral("cue_clear"),
                QStringLiteral("cue_goto"),
                QStringLiteral("cue_gotoandplay"),
                QStringLiteral("cue_gotoandstop"),
                QStringLiteral("cue_cdj"),
                QStringLiteral("cue_play"),
                QStringLiteral("cue_preview"),
                QStringLiteral("play_stutter"),
                // Intro/outro point edits (points live on the shared Track).
                QStringLiteral("intro_start_set"),
                QStringLiteral("intro_start_clear"),
                QStringLiteral("intro_end_set"),
                QStringLiteral("intro_end_clear"),
                QStringLiteral("outro_start_set"),
                QStringLiteral("outro_start_clear"),
                QStringLiteral("outro_end_set"),
                QStringLiteral("outro_end_clear"),
                // Loop commands. `loop_enabled` is deliberately NOT relayed
                // although 5.5 listed it in both directions: it is first a
                // status the leader's engine writes with setAndConfirm, and
                // the follower's own replica does the same while it executes
                // a relayed loop command. Such a status echo looks exactly
                // like a user command to the relay -- on a *toggle*
                // (`reloop_toggle`) it inverts the leader: the follower's
                // exit arrives before the toggle and re-enables what the
                // toggle has just exited. Loop enable/disable commands have
                // dedicated trigger keys (all relayed); the region state
                // flows back on the mirror.
                QStringLiteral("loop_in"),
                QStringLiteral("loop_out"),
                QStringLiteral("loop_exit"),
                QStringLiteral("loop_halve"),
                QStringLiteral("loop_double"),
                QStringLiteral("reloop_toggle"),
                QStringLiteral("reloop_andstop"),
                QStringLiteral("loop_remove"),
                QStringLiteral("loop_anchor"),
                QStringLiteral("loop_scale"),
                // `beatloop_size` / `beatjump_size` relay, but as *deferred
                // value carriers* (BridgeEntry::deferred,
                // DECK_PAIR_PLAN.md 9.19), not inline. They must cross in
                // the first place: the SC Live 4 loop encoder writes the
                // raw size to resize the live pair loop, and with the keys
                // unrelayed (the state of 9.12 a) the follower resized only
                // its own replica while the leader kept the old loop --
                // out of sync after ending loop mode (bench round 7, T45).
                // They must not cross inline either: the engine rewrites
                // them as status whenever a size-carrying command runs
                // (loop_halve halves the size CO), and an inline echo
                // overtakes the bridge's own relay slot for that very
                // command and applies the change a second time --
                // loop_halve from the follower halved 4 -> 1 instead of
                // 4 -> 2 (9.12 a, T19). Delivering the carriers after the
                // event loop turns gives every gesture exactly one
                // carrier: a relayed command lands first and the leader's
                // own engine writes its matching size, so the echo
                // quiesces by equality; a raw encoder write has no
                // command, so it crosses on the next event-loop turn. All
                // size *commands* (beatloop_<size>_activate/toggle,
                // beatjump_size_double/halve, loop_halve/loop_double,
                // loop_scale) stay inline relays as before.
                QStringLiteral("beatloop_size"),
                QStringLiteral("beatjump_size"),
                QStringLiteral("beatloop_activate"),
                QStringLiteral("beatjump_forward"),
                QStringLiteral("beatjump_backward"),
                QStringLiteral("beatjump_size_double"),
                QStringLiteral("beatjump_size_halve"),
                // Scratch / nudge surface. `jog` is deliberately NOT listed:
                // it is the one consumer-zeroed accumulator, and its value
                // only means anything at the consumer's read-and-zero. The
                // pair transport carries it on the accumulator lane instead,
                // published by the follower's gated consume step and claimed
                // by the leader's own jog consumption (5.4.1, 9.13); the
                // consume-and-drop that publishes it runs through
                // EngineBuffer's pair housekeeping (9.12 h).
                // `wheel` (absolute-value rotary: writes set the value, only
                // the engine's TTRotary decay moves it between writes) and
                // `scratch2` (value owned by the scratch source) are plain
                // value copies; `scratch_position` relays as a value too, not
                // an accumulator (5.5).
                QStringLiteral("wheel"),
                QStringLiteral("scratch2"),
                QStringLiteral("scratch2_enable"),
                QStringLiteral("scratch_position"),
                QStringLiteral("scratch_position_enable"),
                // The complete rate-control surface (9.11 d): moving any
                // rate control of either member moves the pair's rate.
                QStringLiteral("rate"),
                QStringLiteral("rate_ratio"),
                QStringLiteral("rateRange"),
                QStringLiteral("rate_dir"),
                QStringLiteral("rate_temp_up"),
                QStringLiteral("rate_temp_down"),
                QStringLiteral("rate_temp_up_small"),
                QStringLiteral("rate_temp_down_small"),
                QStringLiteral("rate_perm_up"),
                QStringLiteral("rate_perm_down"),
                QStringLiteral("rate_perm_up_small"),
                QStringLiteral("rate_perm_down_small"),
                QStringLiteral("reverse"),
                QStringLiteral("fwd"),
                QStringLiteral("back"),
                // Key / pitch.
                QStringLiteral("keylock"),
                QStringLiteral("pitch_adjust"),
                QStringLiteral("keylockMode"),
                QStringLiteral("keyunlockMode"),
                // Tempo taps: relaying lets the leader's BpmControl run the
                // tap for the pair; the resulting tempo also arrives
                // through the rate_ratio net.
                QStringLiteral("bpm_tap"),
                QStringLiteral("tempo_tap"),
                // Hotcue organisation over the shared Track (idempotent).
                QStringLiteral("sort_hotcues"),
                QStringLiteral("sort_hotcues_remove_offsets"),
        };
        // Hotcue commands over the full slot range. (The Phase 2 subset
        // looped 0..7 -- that produced a never-existing hotcue_0_* key and
        // silently dropped hotcue_8 from the bridge; the exists() guard in
        // buildBridge kept it harmless, but the range is 1-based.)
        for (int hotcue = 1; hotcue <= kMaxNumberOfHotcues; ++hotcue) {
            const QString prefix = QStringLiteral("hotcue_%1").arg(hotcue);
            relay << prefix + QStringLiteral("_activate")
                  << prefix + QStringLiteral("_activatecue")
                  << prefix + QStringLiteral("_activateloop")
                  << prefix + QStringLiteral("_activate_preview")
                  << prefix + QStringLiteral("_set")
                  << prefix + QStringLiteral("_setcue")
                  << prefix + QStringLiteral("_setloop")
                  << prefix + QStringLiteral("_goto")
                  << prefix + QStringLiteral("_gotoandplay")
                  << prefix + QStringLiteral("_gotoandstop")
                  << prefix + QStringLiteral("_gotoandloop")
                  << prefix + QStringLiteral("_cueloop")
                  << prefix + QStringLiteral("_clear")
                  << prefix + QStringLiteral("_swap")
                  << prefix + QStringLiteral("_color");
        }
        for (const double size : LoopingControl::getBeatSizes()) {
            relay << QStringLiteral("beatloop_%1_activate").arg(size)
                  << QStringLiteral("beatloop_%1_toggle").arg(size)
                  << QStringLiteral("beatjump_%1_forward").arg(size)
                  << QStringLiteral("beatjump_%1_backward").arg(size)
                  << QStringLiteral("loop_move_%1_forward").arg(size)
                  << QStringLiteral("loop_move_%1_backward").arg(size);
            // NOT: beatlooproll_<size>_activate, beatlooproll_r<size>_activate,
            // beatloop_r<size>_activate/_toggle -- the roll family engages
            // slip and is therefore deck-local (R8).
        }
        return relay;
    }();
    return items;
}

/// Echo guard for bridge writes (bench finding, DECK_PAIR_PLAN.md 9.11 l):
/// while the bridge itself is writing a target control, the relay must not
/// treat the resulting change as a new command. Equality quiescence alone is
/// not enough in production, where mirrors are queued to the main loop while
/// relays run inline on the writer thread: a mirror that lands late carries a
/// stale value, differs from the user's newest one, and would be relayed back
/// to the leader - a feedback loop between two stale-ghost streams which the
/// bench heard as a pitch-fader limit cycle. The marker is per thread: a
/// mirror echo is *delivery*, never a command, while user writes on the GUI or
/// controller thread always see a free (zero) depth.
thread_local int t_bridgeWriteDepth = 0;

class BridgeWriteGuard {
  public:
    BridgeWriteGuard() {
        ++t_bridgeWriteDepth;
    }
    ~BridgeWriteGuard() {
        --t_bridgeWriteDepth;
    }
    BridgeWriteGuard(const BridgeWriteGuard&) = delete;
    BridgeWriteGuard& operator=(const BridgeWriteGuard&) = delete;
};

/// Status keys mirrored from the pair leader onto the pair follower
/// (DECK_PAIR_PLAN.md 5.5, the full Phase 3 table). Deliberately missing:
/// anything slip-related, mixer-strip or stem controls, sync/vinyl,
/// `pair_enabled` itself, and the engine-internal values the follower
/// recomputes coherently at the pair position -- `key`, `pitch`,
/// `beat_distance`, `local_bpm`, `beat_active`, time displays (5.4.6, so
/// `pitch_adjust` and `keylock` are mirrored but the derived `pitch`/`key`
/// displays are not; T22 pins that they agree anyway). `playposition` is
/// the one exception: a locked follower stops writing its own playposition,
/// so the leader's is mirrored to keep it alive for skins and controllers
/// (deviation 9.11 a).
const QStringList& fullMirrorItems() {
    static const QStringList items = []() {
        QStringList mirror = {
                QStringLiteral("play"),
                QStringLiteral("play_indicator"),
                QStringLiteral("play_latched"),
                QStringLiteral("cue_indicator"),
                QStringLiteral("playposition"),
                // The loop points precede loop_enabled on purpose: the bulk
                // refreshes write through the follower's request path, and
                // its LoopingControl refuses a loop enable without a valid
                // region (a cold replica at slip release has none until the
                // points land, 9.11 m). Event-by-event delivery is already
                // ordered this way by the leader's own engine.
                QStringLiteral("loop_start_position"),
                QStringLiteral("loop_end_position"),
                QStringLiteral("loop_enabled"),
                QStringLiteral("loop_anchor"),
                QStringLiteral("beatloop_size"),
                // Size *state* flows both ways: leader selections land on
                // the follower here, follower encoder writes cross on the
                // deferred relay carriers (see the note in
                // fullRelayItems, 9.19). Mirror writes of these keys echo
                // into the relay listener under the write guard and
                // quiesce; the carrier settles the pair because it compares
                // against the leader value it saw at scheduling.
                QStringLiteral("beatjump_size"),
                QStringLiteral("cue_point"),
                QStringLiteral("quantize"),
                QStringLiteral("repeat"),
                QStringLiteral("rate"),
                QStringLiteral("rate_ratio"),
                QStringLiteral("reverse"),
                QStringLiteral("keylock"),
                QStringLiteral("pitch_adjust"),
                // Intro/outro point state (points shared via Track; the COs
                // are per-deck status). Positions before enabled, like the
                // loop points.
                QStringLiteral("intro_start_position"),
                QStringLiteral("intro_start_enabled"),
                QStringLiteral("intro_end_position"),
                QStringLiteral("intro_end_enabled"),
                QStringLiteral("outro_start_position"),
                QStringLiteral("outro_start_enabled"),
                QStringLiteral("outro_end_position"),
                QStringLiteral("outro_end_enabled"),
        };
        for (int hotcue = 1; hotcue <= kMaxNumberOfHotcues; ++hotcue) {
            const QString prefix = QStringLiteral("hotcue_%1").arg(hotcue);
            mirror << prefix + QStringLiteral("_status")
                   << prefix + QStringLiteral("_position")
                   << prefix + QStringLiteral("_endposition")
                   << prefix + QStringLiteral("_type")
                   << prefix + QStringLiteral("_color");
        }
        for (const double size : LoopingControl::getBeatSizes()) {
            mirror << QStringLiteral("beatloop_%1_enabled").arg(size);
        }
        return mirror;
    }();
    return items;
}

/// Status controls which their engine owner declares read-only (they are
/// written through `forceSet`). A plain `set()` would be refused with a
/// warning, so the mirror writes those like the owner itself does -- always
/// quiesced like every other bridge write, so a forced status write never
/// echoes back.
bool isOwnerOnlyMirrorItem(const QString& item) {
    if (item == QLatin1String("play_latched")) {
        return true;
    }
    if (item.startsWith(QLatin1String("hotcue_"))) {
        return item.endsWith(QLatin1String("_status")) ||
                item.endsWith(QLatin1String("_type")) ||
                // Legacy alias of _status (cuecontrol.cpp).
                item.endsWith(QLatin1String("_enabled"));
    }
    if (item.startsWith(QLatin1String("beatloop_"))) {
        return item.endsWith(QLatin1String("_enabled"));
    }
    if (item.startsWith(QLatin1String("intro_")) ||
            item.startsWith(QLatin1String("outro_"))) {
        return item.endsWith(QLatin1String("_enabled"));
    }
    return false;
}

/// `[ChannelN], pair_enabled` is only meaningful for real decks.
bool isPairableDeckGroup(const QString& group) {
    return PlayerManager::isDeckGroup(group);
}

// The "[Controls]" keys of the "load when deck playing" safeguard come from
// preferences/dialog/dlgprefdeck.h, which defines them in an anonymous
// namespace (backport note (2.6): on this branch that header is both the
// canonical definition and the include source, as for playermanager.cpp;
// on 2.7 the enum lives in preferences/interface.h and the keys had to be
// repeated here).

/// The user's safeguard against loading a track onto a playing deck, with the
/// same tri-state and legacy handling as the library load paths (drag & drop,
/// track menu, table view).
LoadWhenDeckPlaying loadWhenDeckPlaying(const UserSettingsPointer& pConfig) {
    if (pConfig->exists(kConfigKeyLoadWhenDeckPlaying)) {
        return static_cast<LoadWhenDeckPlaying>(
                pConfig->getValueString(kConfigKeyLoadWhenDeckPlaying).toInt());
    }
    // Support the older version of this flag.
    return pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck)
            ? LoadWhenDeckPlaying::Allow
            : kDefaultLoadWhenDeckPlaying;
}

bool isPlaying(const ControlProxy* pPlay) {
    return pPlay != nullptr && pPlay->toBool();
}
} // namespace

DeckPairManager::DeckPairManager(UserSettingsPointer pConfig,
        EngineMixer* pEngine,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pEngine(pEngine) {
    m_members.resize(kMaxNumberOfDecks);
    m_pairs.reserve(kNumberOfPairSlots);
    for (int slot = 0; slot < kNumberOfPairSlots; ++slot) {
        auto pPair = std::make_unique<Pair>();
        pPair->slotIndex = slot;
        m_pairs.push_back(std::move(pPair));
    }
}

DeckPairManager::~DeckPairManager() {
    // Disarm everything before the decks (and therefore the engine buffers
    // holding a pointer to our static transports) are destroyed.
    disbandAllPairs();
}

void DeckPairManager::addDeck(int deckIndex, Deck* pDeck) {
    VERIFY_OR_DEBUG_ASSERT(pDeck != nullptr) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(deckIndex >= 0 &&
            deckIndex < static_cast<int>(m_members.size())) {
        qWarning() << Q_FUNC_INFO << "ignoring deck index" << deckIndex;
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(m_members[deckIndex] == nullptr) {
        qWarning() << Q_FUNC_INFO << "deck index" << deckIndex << "is already registered";
        return;
    }
    const QString group = pDeck->getGroup();
    VERIFY_OR_DEBUG_ASSERT(isPairableDeckGroup(group)) {
        qWarning() << Q_FUNC_INFO << group << "is not a deck group";
        return;
    }

    auto pMember = std::make_unique<Member>();
    pMember->deckIndex = deckIndex;
    pMember->group = group;
    pMember->pDeck = pDeck;
    pMember->pEngineBuffer = pDeck->getEngineDeck()->getEngineBuffer();
    VERIFY_OR_DEBUG_ASSERT(pMember->pEngineBuffer != nullptr) {
        return;
    }

    // The pair_enabled control is owned by the manager: every write is
    // arbitrated here, following the [App], num_decks pattern. The value is
    // not persisted: a pair is a runtime-only state.
    pMember->pPairEnabled = std::make_unique<ControlObject>(
            ConfigKey(group, QString::fromLatin1(kPairEnabledItem)));
    pMember->pPairEnabled->setDescription(tr(
            "Act together with the partner deck as a single deck"));
    // Thread affinity: a mapping on a controller thread may write pair_enabled,
    // and the arbitration below forms or disbands a pair -- it creates and
    // destroys the member's ControlProxies, walks the decks and the engine
    // buffers, and touches the bridge tables. None of that may run on a
    // controller thread. AutoConnection keeps a same-thread write (a skin
    // button, the normal case) synchronous, and defers a foreign writer onto
    // the thread the manager lives on, exactly like the leader-side mirror
    // listener in buildBridge() below.
    pMember->pPairEnabled->connectValueChangeRequest(
            this,
            [this, deckIndex](double value) {
                slotPairEnabledChangeRequest(deckIndex, value);
            },
            Qt::AutoConnection);

    pMember->pPlay = make_parented<ControlProxy>(
            group, QStringLiteral("play"), this);
    pMember->pPlayLatched = make_parented<ControlProxy>(
            group, QStringLiteral("play_latched"), this);
    pMember->pPassthrough = make_parented<ControlProxy>(
            group, QStringLiteral("passthrough"), this);
    // Same reasoning as pair_enabled above, and this one is not hypothetical:
    // vinyl control writes passthrough from the controller thread, and
    // slotPassthroughChanged() disbands the pair right there.
    pMember->pPassthrough->connectValueChanged(
            this,
            [this, deckIndex](
                    double value) { slotPassthroughChanged(deckIndex, value); },
            Qt::AutoConnection);

    // Transport authorities of the decks themselves. They exist once the deck
    // has been constructed; missing controls are simply not refused.
    for (const QString& item : kRefusedFollowerAuthorityItems) {
        const ConfigKey key(group, item);
        ControlObject* pControl = ControlObject::getControl(key);
        if (pControl == nullptr) {
            continue;
        }
        // The owner of these controls confirms every write, so a change
        // request is the earliest point at which a refusal can intervene. Our
        // handler runs after the owner confirmed the value.
        pControl->connectValueChangeRequest(
                this,
                [this, deckIndex, item](double value) {
                    slotRefusedAuthorityRequest(deckIndex, item, value);
                },
                Qt::DirectConnection);
        auto pProxy = std::make_unique<ControlProxy>(key, this);
        pProxy->connectValueChanged(
                this,
                [this, group, item](double value) {
                    slotRefusedAuthorityChanged(group, item, value);
                },
                Qt::DirectConnection);
        pMember->pRefusedAuthorities.push_back(std::move(pProxy));
    }
    for (const QString& item : kRefusedFollowerPlainItems) {
        const ConfigKey key(group, item);
        if (!ControlObject::exists(key)) {
            continue;
        }
        auto pProxy = std::make_unique<ControlProxy>(key, this);
        pProxy->connectValueChanged(
                this,
                [this, group, item](double value) {
                    slotRefusedAuthorityChanged(group, item, value);
                },
                Qt::DirectConnection);
        pMember->pRefusedAuthorities.push_back(std::move(pProxy));
    }

    // Load and eject of a paired deck affect both members. loadingTrack()
    // fires for every load entry point (library, QML, AutoDJ, clone, ...).
    connect(pDeck,
            &BaseTrackPlayer::loadingTrack,
            this,
            [this, deckIndex](TrackPointer pNewTrack, TrackPointer pOldTrack) {
                slotLoadingTrack(deckIndex, pNewTrack, pOldTrack);
            });

    kLoggerDeckPair.debug() << Q_FUNC_INFO << deckIndex << group;
    m_members[deckIndex] = std::move(pMember);
}

int DeckPairManager::numberOfDecks() const {
    int count = 0;
    for (const auto& pMember : m_members) {
        if (pMember) {
            ++count;
        }
    }
    return count;
}

// static
int DeckPairManager::partnerDeckIndex(int deckIndex) {
    if (deckIndex < 0 || deckIndex >= kMaxNumberOfDecks) {
        return -1;
    }
    // Decks are paired across the slot distance: 0<->2 and 1<->3.
    const int partner = deckIndex < kPairSlotDistance
            ? deckIndex + kPairSlotDistance
            : deckIndex - kPairSlotDistance;
    return (partner >= 0 && partner < kMaxNumberOfDecks) ? partner : -1;
}

// static
QStringList DeckPairManager::bridgeRelayKeys() {
    return fullRelayItems();
}

// static
QStringList DeckPairManager::bridgeMirrorKeys() {
    return fullMirrorItems();
}

double DeckPairManager::pairAccumulated(const QString& group) const {
    // Test hook (DECK_PAIR_PLAN.md 9.13): the pending `jog` accumulation on
    // the transport lane of the active pair containing \p group. Reads the
    // lane without claiming it -- the claim belongs to the leader's consumer.
    const Member* pMember = member(group);
    if (pMember == nullptr) {
        return 0.0;
    }
    const Pair* pPair = activePairForDeckIndex(pMember->deckIndex);
    if (pPair == nullptr) {
        return 0.0;
    }
    return pPair->transport.accumulated();
}

// static
bool DeckPairManager::isAudible(const Member& member) {
    if (isPlaying(member.pPlay.get())) {
        return true;
    }
    // A latched play covers a running cue preview: the deck is audible and
    // pairing it would hijack its transport. (cue_indicator is deliberately
    // not consulted: it also blinks in the ordinary stopped state.)
    return member.pPlayLatched && member.pPlayLatched->toBool();
}

bool DeckPairManager::isPaired(const QString& group) const {
    const Member* pMember = member(group);
    return pMember != nullptr && activePairForDeckIndex(pMember->deckIndex) != nullptr;
}

PairRole DeckPairManager::pairRole(const QString& group) const {
    const Member* pMember = member(group);
    if (pMember == nullptr) {
        return PairRole::Unpaired;
    }
    const Pair* pPair = activePairForDeckIndex(pMember->deckIndex);
    if (pPair == nullptr) {
        return PairRole::Unpaired;
    }
    return pPair->leaderDeckIndex == pMember->deckIndex ? PairRole::PairLeader
                                                        : PairRole::PairFollower;
}

QString DeckPairManager::partnerGroup(const QString& group) const {
    const Member* pMember = member(group);
    if (pMember == nullptr) {
        return QString();
    }
    const Pair* pPair = activePairForDeckIndex(pMember->deckIndex);
    if (pPair == nullptr) {
        return QString();
    }
    const int partnerIndex = pPair->leaderDeckIndex == pMember->deckIndex
            ? pPair->followerDeckIndex
            : pPair->leaderDeckIndex;
    const Member* pPartner = memberByIndex(partnerIndex);
    return pPartner ? pPartner->group : QString();
}

QString DeckPairManager::pairDescription(const QString& group) const {
    const QString partner = partnerGroup(group);
    if (partner.isEmpty()) {
        return QStringLiteral("%1: unpaired").arg(group);
    }
    return QStringLiteral("%1 (%2): %3")
            .arg(group,
                    pairRole(group) == PairRole::PairLeader ? QStringLiteral("leader")
                                                            : QStringLiteral("follower"),
                    partner);
}

void DeckPairManager::slotPairEnabledChangeRequest(int deckIndex, double value) {
    if (m_bArbitrating) {
        // Our own guarded write to the partner's control.
        return;
    }
    Member* pSelf = memberByIndex(deckIndex);
    if (pSelf == nullptr) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(pSelf->pPairEnabled) {
        return;
    }

    if (value <= 0.0) {
        Pair* pPair = activePairForDeckIndex(deckIndex);
        if (pPair != nullptr) {
            disbandPair(*pPair, pSelf->group);
        }
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    // --- pairing request ---
    Pair* pSlot = pairForDeckIndex(deckIndex);
    if (pSlot == nullptr) {
        // No partner deck index at all (decks beyond 4).
        kLoggerDeckPair.warning() << pSelf->group
                                  << "has no partner deck, refusing to pair";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }
    if (pSlot->active) {
        // A pair slot only contains these two decks, so an active slot means
        // this very deck is a member of that pair: the write is redundant and
        // must not reset the partner's pair_enabled.
        kLoggerDeckPair.debug() << pSelf->group << "is already paired";
        pSelf->pPairEnabled->setAndConfirm(1.0);
        return;
    }

    const int partnerIndex = partnerDeckIndex(deckIndex);
    Member* pPartner = memberByIndex(partnerIndex);
    if (pPartner == nullptr) {
        kLoggerDeckPair.warning() << pSelf->group << "refuses to pair:"
                                  << "partner deck" << (partnerIndex + 1)
                                  << "does not exist";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    if (isAudible(*pPartner)) {
        kLoggerDeckPair.warning() << pSelf->group << "refuses to pair with deck"
                                  << (partnerIndex + 1)
                                  << "because it is currently playing";
        pSelf->pPairEnabled->setAndConfirm(0.0);
        return;
    }

    for (const Member* pCandidate : {pSelf, pPartner}) {
        if (pCandidate->pPassthrough && pCandidate->pPassthrough->toBool()) {
            kLoggerDeckPair.warning() << pCandidate->group
                                      << "refuses to pair: passthrough is active";
            pSelf->pPairEnabled->setAndConfirm(0.0);
            return;
        }
    }

    // Confirm the initiating write: like [App], num_decks the value only
    // lands when the owner of the control confirms it.
    pSelf->pPairEnabled->setAndConfirm(1.0);
    formPair(*pSelf, *pPartner);
}

void DeckPairManager::formPair(Member& leader, Member& follower) {
    Pair* pPair = pairForDeckIndex(leader.deckIndex);
    VERIFY_OR_DEBUG_ASSERT(pPair != nullptr && !pPair->active) {
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(pPair == pairForDeckIndex(follower.deckIndex)) {
        return;
    }

    kLoggerDeckPair.info() << "Pairing" << leader.group << "(leader) with"
                           << follower.group << "(follower)";

    pPair->leaderDeckIndex = leader.deckIndex;
    pPair->followerDeckIndex = follower.deckIndex;

    // A follower which enters the pair with sync or vinyl control already
    // active has to give up that authority now, not only on future writes:
    // its tempo and transport come from the pair leader (5.8).
    neutralizeFollowerAuthorities(follower);

    // Hand the shared transport over to the buffers. The objects live as long
    // as the manager, so the pointers can never dangle while flags say Unpaired.
    pPair->transport.setRoles(PairRole::PairLeader, PairRole::PairFollower);
    leader.pEngineBuffer->setPairTransport(&pPair->transport, PairRole::PairLeader);
    follower.pEngineBuffer->setPairTransport(&pPair->transport, PairRole::PairFollower);
    pPair->pFollowerBuffer = follower.pEngineBuffer;
    pPair->pLeaderBuffer = leader.pEngineBuffer;
    pPair->transport.setActive(true);
    pPair->active = true;

    m_bArbitrating = true;
    follower.pPairEnabled->setAndConfirm(1.0);
    // The leader's own write is confirmed by the caller.
    m_bArbitrating = false;

    synchronizeTracks(leader, follower);

    // The bridge goes up after the track adoption: the adoption load writes
    // its own play state and must not be relayed around while it runs.
    buildBridge(*pPair, leader, follower);

    emit pairEnabled(leader.group, follower.group);
}

void DeckPairManager::disbandPair(Pair& pair, const QString& requestingGroup) {
    Member* pLeader = memberByIndex(pair.leaderDeckIndex);
    Member* pFollower = memberByIndex(pair.followerDeckIndex);
    kLoggerDeckPair.info() << "Unpairing" << requestingGroup;

    // Teardown order (DECK_PAIR_PLAN.md 5.1): stop the bridge (Phase 3), clear
    // the pair-active flag so the engine stops publishing/consuming with the
    // next callback, release the follower's slip-plane feed (Phase 4), restore
    // the follower's sync/vinyl acceptability. No position writes are needed:
    // the follower's position already is the pair plane.
    disarmPair(pair);

    m_bArbitrating = true;
    if (pLeader && pLeader->pPairEnabled) {
        pLeader->pPairEnabled->setAndConfirm(0.0);
    }
    if (pFollower && pFollower->pPairEnabled) {
        pFollower->pPairEnabled->setAndConfirm(0.0);
    }
    m_bArbitrating = false;

    const QString leaderGroup = pLeader ? pLeader->group : QString();
    const QString followerGroup = pFollower ? pFollower->group : QString();
    emit pairDisabled(leaderGroup, followerGroup);
}

void DeckPairManager::disarmPair(Pair& pair) {
    Member* pLeader = memberByIndex(pair.leaderDeckIndex);
    Member* pFollower = memberByIndex(pair.followerDeckIndex);

    // Teardown order (5.1): stop the bridge first, then clear the
    // engine-visible flags, then drop the pointer handoff.
    //
    // Retire the bridge generation *before* anything is freed: a queued mirror
    // invocation still holding a Pair*/BridgeEntry* is then recognised as stale
    // instead of being dereferenced. pair.active is only cleared at the end of
    // this function, so before this line the slots' own guard would have waved a
    // stale delivery straight through to freed entries.
    ++pair.bridgeGeneration;
    clearBridge(pair);

    pair.transport.setActive(false);
    if (pLeader && pLeader->pEngineBuffer) {
        pLeader->pEngineBuffer->setPairTransport(nullptr, PairRole::Unpaired);
    }
    if (pFollower && pFollower->pEngineBuffer) {
        pFollower->pEngineBuffer->setPairTransport(nullptr, PairRole::Unpaired);
    }
    pair.transport.reset();

    pair.leaderDeckIndex = -1;
    pair.followerDeckIndex = -1;
    pair.pFollowerBuffer = nullptr;
    pair.pLeaderBuffer = nullptr;
    pair.active = false;
}

void DeckPairManager::disbandAllPairs() {
    for (const auto& pPair : m_pairs) {
        if (pPair->active) {
            m_bDisbanding = true;
            disbandPair(*pPair, QStringLiteral("(shutdown)"));
            m_bDisbanding = false;
        }
    }
}

void DeckPairManager::synchronizeTracks(Member& leader, Member& follower) {
    // No "load when deck playing" check is needed here: pairing a deck which
    // is audible is refused outright (R5), so the deck adopting a track is
    // never playing at formation -- neither the follower (the leader's track
    // is adopted) nor an empty leader (it holds no track to play).
    TrackPointer pLeaderTrack = leader.pDeck->getLoadedTrack();
    TrackPointer pFollowerTrack = follower.pDeck->getLoadedTrack();

    m_bPropagatingLoad = true;
    if (pLeaderTrack) {
        if (pLeaderTrack != pFollowerTrack) {
            // The leader's track is the pair's track (R3). Reuse the clone
            // path so the first buffer is aligned, but keep the follower's own
            // stem selection: stem mute/solo/gain are strictly per deck (R9).
            kLoggerDeckPair.debug() << "Follower" << follower.group
                                    << "adopts the track of leader" << leader.group;
            // cloneFromChannel() takes the track from the source deck's
            // *engine buffer* and returns silently while a load is still in
            // flight there. That is a real state, not a corner case: unpair
            // and re-pair straight away, or pair a deck whose load has just
            // been requested. A silent no-op would leave the pair locked
            // across two tracks, so fall back to a plain load of the deck's
            // own track pointer -- the clone only adds frame alignment of the
            // first buffer, which needs a loaded source anyway and is
            // unnecessary while the pair is not running yet.
            // Backport note (2.6): on this branch EngineBuffer::getLoadedTrack()
            // keeps reporting the *previous* track while a new load is in
            // flight (2.7 reports the requested track already). A mere
            // "not null" check would therefore clone the stale previous track
            // onto the partner; readiness must mean "the engine finished
            // loading *this* deck's current track". Equivalent on 2.7.
            const bool leaderEngineReady = leader.pEngineBuffer != nullptr &&
                    leader.pEngineBuffer->getLoadedTrack() == pLeaderTrack;
            if (leaderEngineReady) {
                follower.pDeck->cloneFromChannel(
                        leader.pDeck->getEngineDeck(),
#ifdef __STEM__
                        follower.pEngineBuffer->getStemMask(),
#endif
                        isPlaying(leader.pPlay.get()));
            } else {
                kLoggerDeckPair.debug()
                        << "Leader" << leader.group
                        << "is still loading; adopting its track with a plain "
                           "load instead of a clone PROBE_FALLBACK_RAN";
#ifdef __STEM__
                follower.pDeck->slotLoadTrack(pLeaderTrack,
                        follower.pEngineBuffer->getStemMask(),
                        isPlaying(leader.pPlay.get()));
#else
                follower.pDeck->slotLoadTrack(pLeaderTrack,
                        isPlaying(leader.pPlay.get()));
#endif
            }
        }
    } else if (pFollowerTrack) {
        // An empty leader takes over the follower's track instead of ejecting
        // it: the pair stays loadable and no loaded track is lost. Same
        // ready-check as above.
        kLoggerDeckPair.debug() << "Empty leader" << leader.group
                                << "adopts the track of follower" << follower.group;
        // Same ready-check as above: on this branch the engine buffer reports
        // the previous track while a load is in flight, so readiness must mean
        // "the engine loaded *this* deck's current track" (backport note).
        const bool followerEngineReady = follower.pEngineBuffer != nullptr &&
                follower.pEngineBuffer->getLoadedTrack() == pFollowerTrack;
        if (followerEngineReady) {
            leader.pDeck->cloneFromChannel(
                    follower.pDeck->getEngineDeck(),
#ifdef __STEM__
                    leader.pEngineBuffer->getStemMask(),
#endif
                    false);
        } else {
#ifdef __STEM__
            leader.pDeck->slotLoadTrack(pFollowerTrack,
                    leader.pEngineBuffer->getStemMask(),
                    false);
#else
            leader.pDeck->slotLoadTrack(pFollowerTrack, false);
#endif
        }
    }
    m_bPropagatingLoad = false;

    VERIFY_OR_DEBUG_ASSERT(leader.pDeck->getLoadedTrack() ==
            follower.pDeck->getLoadedTrack()) {
        kLoggerDeckPair.warning() << "Pair members" << leader.group << follower.group
                                  << "do not hold the same track";
    }
}

void DeckPairManager::slotLoadingTrack(
        int deckIndex, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    if (m_bPropagatingLoad) {
        return;
    }
    if (!pNewTrack && !pOldTrack) {
        // Neither a load nor an eject.
        return;
    }

    const Member* pSource = memberByIndex(deckIndex);
    if (pSource == nullptr) {
        return;
    }
    const Pair* pPair = activePairForDeckIndex(deckIndex);
    if (pPair == nullptr) {
        return;
    }
    const int otherIndex = pPair->leaderDeckIndex == deckIndex
            ? pPair->followerDeckIndex
            : pPair->leaderDeckIndex;
    Member* pTarget = memberByIndex(otherIndex);
    if (pTarget == nullptr) {
        return;
    }

    // A load or an eject on any member affects both decks (R7). The leader's
    // transport remains the authority: a load on the follower is applied to
    // the leader as well, which restarts its transport on the new track.
    kLoggerDeckPair.debug() << "Propagating"
                            << (pNewTrack ? "load of" : "eject from")
                            << pSource->group << "to" << pTarget->group;
    propagateTrack(*pTarget, pNewTrack);
}

void DeckPairManager::propagateTrack(Member& target, TrackPointer pTrack) {
    TrackPointer pTargetTrack = target.pDeck->getLoadedTrack();
    if (pTrack == pTargetTrack) {
        return;
    }

    // The user's safeguard against touching a deck that is playing applies to
    // a partner deck as well: a pair must not be a way to get around it. Both
    // a propagated load and a propagated eject go through the same tri-state
    // rule and the legacy flag behind it.
    //
    // A refusal cannot simply be skipped. The pair is still locked, and a
    // locked follower replays the leader's *frame positions* against whatever
    // track it holds itself: two unrelated tracks at one transport, with
    // nothing to detect or heal it. The propagation failed because the user's
    // request ran into a safeguard on the *other* deck, and the user's request
    // outranks the pair -- so the pair is disbanded instead. Both decks
    // survive with a coherent track, which is also what keeps the same-track
    // invariant of 5.7 true for an active pair. Same resolution as the
    // passthrough interlock (5.8).
    //
    // The intended end state remains all-or-nothing (the pair operation
    // completes on both decks or is refused on both), which needs a pre-load
    // veto rather than a post-refusal repair: bench item, DECK_PAIR_PLAN.md 2
    // and 9.7.
    bool playAfterLoad = false;
    if (isPlaying(target.pPlay.get())) {
        switch (loadWhenDeckPlaying(m_pConfig)) {
        case LoadWhenDeckPlaying::Reject: {
            const char* const action = pTrack
                    ? "loading a track onto it is refused"
                    : "ejecting it is refused";
            kLoggerDeckPair.warning()
                    << target.group << "is playing:" << action
                    << " (Preferences > Decks > Load when deck playing)."
                    << " The pair is disbanded so that both decks keep a"
                    << " coherent track.";
            // Keyed on a divergence that is real, not on the play state that
            // raised the refusal: on a locked follower the play CO is a mirror
            // and the mirror can lag (an engine-side play release reaches it via
            // an auto connection), so "partner reads as playing" is not by
            // itself evidence of an inconsistent pair. It does not need to be:
            // the early return above drops the case where both members already
            // hold the track being propagated, so reaching this line always
            // means the source deck moved to a track the partner is being
            // refused -- which is precisely the two-tracks-at-one-transport
            // state the disband exists to prevent. Auto DJ loading the next
            // track as a pair hits the end of the song is that case, and the
            // pair has genuinely diverged there.
            Pair* pPair = activePairForDeckIndex(target.deckIndex);
            if (pPair != nullptr) {
                disbandPair(*pPair, target.group);
            }
            return;
        }
        case LoadWhenDeckPlaying::Allow:
            // A load keeps playing the new track from its load point; an eject
            // leaves the deck stopped either way.
            playAfterLoad = static_cast<bool>(pTrack);
            break;
        case LoadWhenDeckPlaying::AllowButStopDeck:
            // Loading stops the deck: unloadTrack() releases play for us.
            break;
        }
    }

    m_bPropagatingLoad = true;
    if (!pTrack) {
        // Eject both. slotEjectTrack() refuses while playing, so the partner is
        // stopped first -- reached only when the safeguard above allows it.
        if (isPlaying(target.pPlay.get())) {
            stopDeck(target.pDeck);
        }
        target.pDeck->slotEjectTrack(1.0);
    } else {
#ifdef __STEM__
        const auto stemMask = target.pEngineBuffer
                ? target.pEngineBuffer->getStemMask()
                : mixxx::StemChannelSelection();
#endif
        target.pDeck->slotLoadTrack(pTrack,
#ifdef __STEM__
                stemMask,
#endif
                playAfterLoad);
    }
    m_bPropagatingLoad = false;
}

void DeckPairManager::slotRefusedAuthorityRequest(
        int deckIndex, const QString& item, double value) {
    if (value <= 0.0) {
        // Disabling is always allowed.
        return;
    }
    Member* pMember = memberByIndex(deckIndex);
    if (pMember == nullptr || !isLockedFollower(*pMember)) {
        return;
    }

    // The deck is a pair follower: its tempo and its transport come from the
    // pair leader. A follower-side tempo correction would be applied twice,
    // and an active follower would negotiate leadership for what is a single
    // transport, so the request is refused. The owner of the control has
    // already confirmed the value at this point, therefore the pending engine
    // request is undone and the control is snapped back to 0.
    refuseAuthority(*pMember, item);
}

void DeckPairManager::slotRefusedAuthorityChanged(
        const QString& group, const QString& item, double value) {
    if (value <= 0.0) {
        return;
    }
    Member* pMember = member(group);
    if (pMember == nullptr || !isLockedFollower(*pMember)) {
        return;
    }
    if (!kRefusedFollowerPlainItems.contains(item)) {
        // Confirmed controls are handled where the request arrives; this hook
        // only catches values which the engine itself wrote to the follower.
        ControlObject* pControl = ControlObject::getControl(ConfigKey(group, item));
        if (pControl != nullptr) {
            kLoggerDeckPair.warning() << group << "," << item
                                      << "was activated on a pair follower -- disabled";
            pControl->set(0.0);
        }
        return;
    }
    refuseAuthority(*pMember, item);
}

void DeckPairManager::refuseAuthority(Member& member, const QString& item) {
    VERIFY_OR_DEBUG_ASSERT(isLockedFollower(member)) {
        return;
    }
    kLoggerDeckPair.warning() << member.group << "," << item
                              << "is not available on a pair follower -- disabled";

    if (isSyncAuthorityItem(item)) {
        // Undo the sync request the owner queued on our behalf. Without this
        // the deck would join the sync group for one engine callback.
        member.pEngineBuffer->requestSyncMode(SyncMode::None);
    }

    ControlObject* pControl = ControlObject::getControl(
            ConfigKey(member.group, item));
    if (pControl != nullptr) {
        // Snap back. Plain set() goes through the owner which confirms the
        // value, so both the control and its engine state end up at 0.
        pControl->set(0.0);
    }
}

void DeckPairManager::slotPassthroughChanged(int deckIndex, double value) {
    if (value <= 0.0 || m_bDisbanding) {
        return;
    }
    // EngineDeck::process() short-circuits before EngineBuffer::process() while
    // passthrough is active, so the shared transport would freeze. Passthrough
    // and deck pairs are mutually exclusive: the pair is disbanded rather than
    // silently swallowing the user's passthrough request.
    Pair* pPair = activePairForDeckIndex(deckIndex);
    if (pPair == nullptr) {
        return;
    }
    const Member* pMember = memberByIndex(deckIndex);
    kLoggerDeckPair.warning() << (pMember ? pMember->group : QString())
                              << "enabled passthrough: disbanding the deck pair";
    m_bDisbanding = true;
    disbandPair(*pPair, pMember ? pMember->group : QString());
    m_bDisbanding = false;
}

DeckPairManager::Member* DeckPairManager::member(const QString& group) const {
    for (const auto& pMember : m_members) {
        if (pMember && pMember->group == group) {
            return pMember.get();
        }
    }
    return nullptr;
}

DeckPairManager::Member* DeckPairManager::memberByIndex(int deckIndex) const {
    if (deckIndex < 0 || deckIndex >= static_cast<int>(m_members.size())) {
        return nullptr;
    }
    return m_members[deckIndex].get();
}

DeckPairManager::Pair* DeckPairManager::pairForDeckIndex(int deckIndex) const {
    const int partner = partnerDeckIndex(deckIndex);
    if (partner < 0) {
        return nullptr;
    }
    const int slot = deckIndex % kPairSlotDistance;
    VERIFY_OR_DEBUG_ASSERT(slot >= 0 && slot < static_cast<int>(m_pairs.size())) {
        return nullptr;
    }
    return m_pairs[slot].get();
}

DeckPairManager::Pair* DeckPairManager::activePairForDeckIndex(int deckIndex) const {
    Pair* pPair = pairForDeckIndex(deckIndex);
    if (pPair == nullptr || !pPair->active) {
        return nullptr;
    }
    if (pPair->leaderDeckIndex != deckIndex && pPair->followerDeckIndex != deckIndex) {
        return nullptr;
    }
    return pPair;
}

bool DeckPairManager::isLockedFollower(const Member& member) const {
    return pairRole(member.group) == PairRole::PairFollower;
}

// static
void DeckPairManager::stopDeck(Deck* pDeck) {
    VERIFY_OR_DEBUG_ASSERT(pDeck != nullptr) {
        return;
    }
    ControlObject::set(ConfigKey(pDeck->getGroup(), QStringLiteral("play")), 0.0);
}

void DeckPairManager::buildBridge(
        Pair& pair, Member& leader, Member& follower) {
    VERIFY_OR_DEBUG_ASSERT(pair.active) {
        return;
    }
    // New bridge, new generation: anything still queued from a previous bridge
    // of this slot is stale by construction, and this happens before the
    // entries below capture the value.
    ++pair.bridgeGeneration;
    if (pair.pFollowerSlipEnabled == nullptr) {
        const ConfigKey slipKey(
                follower.group, QStringLiteral("slip_enabled"));
        if (ControlObject::exists(slipKey)) {
            pair.pFollowerSlipEnabled =
                    std::make_unique<ControlProxy>(slipKey, this);
            // Slip release re-anchors the follower's replica: the bridge was
            // fully disconnected while slipping (9.11 m), so adopt the
            // leader's current statuses once, exactly like at formation.
            //
            // Default (auto) connection, not DirectConnection: slip_enabled
            // also changes from other threads -- a load completion resets it
            // from the load worker thread (EngineBuffer::slotTrackLoaded), a
            // hardware roll release from the controller thread -- and the
            // bridge is main-thread machinery (9.11 h); a cross-thread
            // release is therefore delivered on the main loop. From the main
            // thread (GUI, unit tests) delivery stays direct and synchronous.
            Pair* pPair = &pair;
            pair.pFollowerSlipEnabled->connectValueChanged(
                    this,
                    [this, pPair](double value) {
                        if (value != 0.0 || !pPair->active) {
                            return;
                        }
                        // A queued delivery can land after the DJ has
                        // pressed the next roll or slice: re-anchoring a
                        // *slipping* follower is exactly the clobbering this
                        // listener exists to undo, so leave the replica to
                        // the next release. The same holds while the LEADER
                        // slips: its statuses are its own roll gesture, so
                        // the anchor waits for the last slip in the pair
                        // to release (9.16).
                        if (pairMemberSlipping(*pPair)) {
                            return;
                        }
                        refreshMirrorValues(*pPair);
                        // Trailing pass (DECK_PAIR_PLAN.md 9.13): a slip
                        // release can be one write *inside* a longer command
                        // transaction -- the roll release runs loop-disable,
                        // slip-release, loop-restore in one handler -- and an
                        // inline delivery re-anchors in the middle of it, so
                        // the transaction's remaining writes land after the
                        // fresh anchor and clobber it. Re-run the (idempotent,
                        // guard-quiesced) anchor once the event loop turns,
                        // when the command has completed; on the engine and
                        // controller threads the delivery is already queued
                        // behind their current work, so this only closes the
                        // same-thread hole.
                        QTimer::singleShot(0, this, [this, pPair]() {
                            if (!pPair->active) {
                                return;
                            }
                            if (pairMemberSlipping(*pPair)) {
                                return;
                            }
                            refreshMirrorValues(*pPair);
                        });
                    });
        }
    }
    if (pair.pLeaderSlipEnabled == nullptr) {
        const ConfigKey slipKey(
                leader.group, QStringLiteral("slip_enabled"));
        if (ControlObject::exists(slipKey)) {
            pair.pLeaderSlipEnabled =
                    std::make_unique<ControlProxy>(slipKey, this);
            // Leader-side counterpart of the follower listener above
            // (bench round 6, 9.16): the mirror was halted while the leader
            // slipped, so its replica adopts the leader's converged loop
            // status once the slip releases. Same delivery contract (auto
            // connection, main loop), same guards: skip while any member is
            // still slipping -- the last release in the pair performs the
            // re-anchor -- and the trailing pass re-runs it after the
            // release transaction (loop-disable, slip-release, loop-restore)
            // has completed.
            Pair* pPair = &pair;
            pair.pLeaderSlipEnabled->connectValueChanged(
                    this,
                    [this, pPair](double value) {
                        if (value != 0.0 || !pPair->active) {
                            return;
                        }
                        if (pairMemberSlipping(*pPair)) {
                            return;
                        }
                        refreshMirrorValues(*pPair);
                        QTimer::singleShot(0, this, [this, pPair]() {
                            if (!pPair->active) {
                                return;
                            }
                            if (pairMemberSlipping(*pPair)) {
                                return;
                            }
                            refreshMirrorValues(*pPair);
                        });
                    });
        }
    }

    auto addEntries = [this, &pair, &follower, &leader](
                              std::vector<std::unique_ptr<BridgeEntry>>& entries,
                              const QStringList& items,
                              bool relay) {
        for (const QString& item : items) {
            const ConfigKey followerKey(follower.group, item);
            const ConfigKey leaderKey(leader.group, item);
            if (!ControlObject::exists(followerKey) ||
                    !ControlObject::exists(leaderKey)) {
                continue;
            }
            auto entry = std::make_unique<BridgeEntry>();
            entry->item = item;
            entry->forced = !relay && isOwnerOnlyMirrorItem(item);
            entry->deferred = relay && isDeferredRelayItem(item);
            entry->pFollower = std::make_unique<ControlProxy>(followerKey);
            entry->pLeader = std::make_unique<ControlProxy>(leaderKey);
            BridgeEntry* pEntry = entry.get();
            Pair* pPair = &pair;
            if (relay) {
                // User commands arrive synchronously on the writing main
                // thread and reach the leader with zero added latency (3.2).
                const quint64 generation = pPair->bridgeGeneration;
                entry->pFollower->connectValueChanged(
                        this,
                        [this, pPair, pEntry, generation](double value) {
                            slotBridgeRelay(pPair, pEntry, value, generation);
                        },
                        Qt::DirectConnection);
            } else {
                // Statuses are display-rate values; engine-originated writes
                // from the leader are delivered on the main thread with the
                // default (auto) connection.
                const quint64 generation = pPair->bridgeGeneration;
                entry->pLeader->connectValueChanged(
                        this,
                        [this, pPair, pEntry, generation](double value) {
                            slotBridgeMirror(pPair, pEntry, value, generation);
                        },
                        Qt::AutoConnection);
            }
            entries.push_back(std::move(entry));
        }
    };
    addEntries(pair.relays, fullRelayItems(), true);
    addEntries(pair.mirrors, fullMirrorItems(), false);

    // Initial coherence: adopt the leader's current statuses once. The
    // relayed values would otherwise only converge on the next change.
    refreshMirrorValues(pair);

    kLoggerDeckPair.debug() << "Bridge up:" << pair.relays.size() << "relays,"
                            << pair.mirrors.size() << "mirrors";
}

// static
bool DeckPairManager::pairMemberSlipping(const Pair& pair) {
    return (pair.pFollowerSlipEnabled && pair.pFollowerSlipEnabled->toBool()) ||
            (pair.pLeaderSlipEnabled && pair.pLeaderSlipEnabled->toBool());
}

void DeckPairManager::refreshMirrorValues(Pair& pair) {
    // While any member slips, the leader's loop-family statuses and
    // playposition are slip-gesture state, not the pair's truth: the anchor
    // waits for the last release in the pair -- whichever slip listener
    // fires last performs it (9.11 m, 9.16).
    if (pairMemberSlipping(pair)) {
        return;
    }
    // All of these writes quiesce on their own: target-equal writes are
    // dropped by the bridge slots, and every other echo finds the value it
    // just received already set on its target (see slotBridgeRelay).
    const BridgeWriteGuard refreshGuard;
    for (const auto& pEntry : pair.mirrors) {
        if (pEntry->forced) {
            forceMirrorValue(activePairMemberGroup(pair.followerDeckIndex),
                    pEntry->item,
                    pEntry->pLeader->get());
        } else {
            pEntry->pFollower->set(pEntry->pLeader->get());
        }
    }
}

void DeckPairManager::clearBridge(Pair& pair) {
    // ControlProxies are destroyed on the thread that created them: formation
    // and teardown both run on the main thread.
    pair.relays.clear();
    pair.mirrors.clear();
    pair.pFollowerSlipEnabled.reset();
    pair.pLeaderSlipEnabled.reset();
}

void DeckPairManager::checkSameTrackInvariant(const Pair& pair) const {
    // DECK_PAIR_PLAN.md 5.7: paired members must hold the same track. A
    // locked follower replays the leader's positions against its own track, so
    // a divergence is not cosmetic -- it is two different tracks at one
    // transport. Every load and eject funnels through loadingTrack() and a
    // refused propagation disbands the pair, so reaching this means a funnel
    // was missed; debug-assert it and say so loudly, without changing
    // behaviour in a release build.
    //
    // Main thread only. This is reached from the bridge, and the relay side of
    // the bridge is deliberately *not* main-thread-only: a hardware tempo fader
    // or hotcue pad delivers its write on the controller engine thread and runs
    // the relay inline. Reading Deck::getLoadedTrack() there would copy a
    // plain, non-atomic std::shared_ptr that the main thread assigns during a
    // load, i.e. the assertion would itself be the race it is looking for. Skip
    // those calls: a controller write cannot diverge the pair's tracks, and the
    // main-thread paths that can are still covered.
    if (QThread::currentThread() != thread()) {
        return;
    }
    if (m_bPropagatingLoad || !pair.active) {
        return;
    }
    const Member* pLeader = memberByIndex(pair.leaderDeckIndex);
    const Member* pFollower = memberByIndex(pair.followerDeckIndex);
    if (pLeader == nullptr || pFollower == nullptr ||
            pLeader->pDeck == nullptr || pFollower->pDeck == nullptr) {
        return;
    }
    const TrackPointer pLeaderTrack = pLeader->pDeck->getLoadedTrack();
    if (pLeaderTrack == pFollower->pDeck->getLoadedTrack()) {
        return;
    }
    DEBUG_ASSERT(false);
    kLoggerDeckPair.warning()
            << "Pair members" << pLeader->group << pFollower->group
            << "hold different tracks while paired -- a load or eject bypassed "
               "the pair's load funnel";
}

void DeckPairManager::slotBridgeRelay(
        Pair* pPair, BridgeEntry* pEntry, double value, quint64 generation) {
    // Stale bridge (disbanded, or re-formed since this callback was wired up,
    // or a queued delivery outliving the teardown): pEntry may already be
    // freed, so nothing may be touched before this check.
    if (!pPair->active || pPair->bridgeGeneration != generation) {
        return;
    }
    // Commands only: a CO write seen from inside the audio callback is the
    // follower's own engine machinery (status, replica, indicator updates),
    // never a pair command. Writes from every other thread - the GUI and,
    // crucially, the controller engine thread, which delivers hardware input
    // exactly like it delivers to an unpaired deck - are user commands and
    // are relayed inline (DECK_PAIR_PLAN.md 5.5 / 9.11 h). In the unit tests
    // the callback runs on the main thread; the callback flag keeps the
    // distinction there as well.
    if (m_pEngine != nullptr && m_pEngine->isEngineCallbackThread()) {
        return;
    }
    // An echo of the bridge's own write on this thread is never a command
    // (BridgeWriteGuard, 9.11 l): this is how the pitch-fader limit cycle of
    // the first bench test happened.
    if (t_bridgeWriteDepth > 0) {
        return;
    }
    // Paired members hold the same track (5.7). After the filters above, so the
    // per-hardware-tick path that relays its own echo stays as short as it was.
    checkSameTrackInvariant(*pPair);
    // Slip disconnects the command bridge: a slipping follower drives its
    // own transport and its commands stay local (R8).
    if (pPair->pFollowerSlipEnabled && pPair->pFollowerSlipEnabled->toBool()) {
        return;
    }
    // Mid-roll loop adoption: slipQuitAndAdopt() clears the follower's
    // slip_enabled synchronously, but the engine has not yet consumed the
    // request; treat that window as slipping so the beatloop_activate (and
    // any loop/slip writes) it produces stay on the follower alone
    // (DECK_PAIR_PLAN.md R8 / T30).
    if (pPair->pFollowerBuffer != nullptr &&
            pPair->pFollowerBuffer->isSlipQuitAndAdoptPending()) {
        return;
    }
    // Deferred carrier (9.19): the size keys never write the leader from
    // this callback -- that is exactly how the echo of a relayed gesture
    // overtakes the gesture and doubles it (9.12 a). Schedule one
    // coalesced delivery instead. The carrier stores the *scheduled* value
    // together with the leader's value at scheduling: at delivery the
    // follower may already have re-confirmed its size from its live loop
    // grid (the engine's setAndConfirm path), so reading the follower
    // "live" would lose a raw encoder write entirely -- bench round 7
    // showed the pair never adopting the encoder value. Delivery re-runs
    // the slip gates and the staleness checks, and the leader baseline
    // rejects a carrier whose value the leader has since superseded.
    if (pEntry->deferred) {
        pEntry->deferredValueBits.storeRelease(doubleToBits(value));
        pEntry->deferredLeaderAtScheduleBits.storeRelease(
                doubleToBits(pEntry->pLeader->get()));
        if (pPair->deferredRelayPending.fetchAndStoreOrdered(1) == 0) {
            QTimer::singleShot(0, this, [this, pPair, generation]() {
                deliverDeferredRelays(pPair, generation);
            });
        }
        return;
    }
    // `jog` is not bridged at all (see fullRelayItems): the accumulator's
    // meaning is created by its consumer's read-and-zero, so it crosses on the
    // pair transport's accumulator lane, read at the follower's gated consume
    // step and claimed at the leader's own jog consumption. If a table ever
    // lists it again the copy-relay below would double- or triple-feed nudges
    // on real hardware (bench findings 9.12 i / 9.13) while every unit test
    // that writes once per callback stays green; the T16 burst test exists for
    // precisely that gap.
    // Quiescence by equality (plus the echo guard above): a bridged write
    // makes the target equal to its source, so a same-value echo is dropped.
    // This is what stops the trigger buttons -- which have ignoreNops ==
    // false and re-emit on every write -- from ping-ponging the pair
    // (DECK_PAIR_PLAN.md 5.5, T37).
    if (pEntry->pLeader->get() == value) {
        return;
    }
    const BridgeWriteGuard writeGuard;
    pEntry->pLeader->set(value);
}

// static
bool DeckPairManager::isDeferredRelayItem(const QString& item) {
    // The two size controls carry a value with dual meaning: a parameter
    // for the next loop/jump and, while a loop is live, a resize of that
    // loop. They are the only relay keys that the engine also rewrites as
    // status, which is why they cross deferred (9.19, 9.12 a).
    return item == QLatin1String("beatloop_size") ||
            item == QLatin1String("beatjump_size");
}

void DeckPairManager::deliverDeferredRelays(
        Pair* pPair, quint64 bridgeGeneration) {
    // Clear the schedule flag before reading anything: a size write that
    // lands while this delivery runs re-schedules it instead of getting
    // lost behind a flag that is still set.
    pPair->deferredRelayPending.storeRelease(0);
    // Staleness rules first -- pEntry may already be freed (9.18 residual,
    // same contract as the queued mirror): nothing may be touched before
    // these checks. The bridge generation rejects teardown and re-forming.
    if (!pPair->active || pPair->bridgeGeneration != bridgeGeneration) {
        return;
    }
    // The slip gates that refused the scheduling event apply at delivery
    // too: a roll pad may have engaged in the meantime, and then the size
    // on the follower is roll state, not a pair command (R8).
    if (pPair->pFollowerSlipEnabled && pPair->pFollowerSlipEnabled->toBool()) {
        return;
    }
    if (pPair->pFollowerBuffer != nullptr &&
            pPair->pFollowerBuffer->isSlipQuitAndAdoptPending()) {
        return;
    }
    for (const auto& pEntry : pPair->relays) {
        if (!pEntry->deferred) {
            continue;
        }
        // The *scheduled* value, not the follower's live one: the
        // follower's engine re-confirms the size from its loop grid
        // before this carrier fires (bench round 7), so the live read
        // would deliver the re-confirmed old value and the encoder turn
        // would be lost. Coalescing still holds: every scheduling since
        // the last delivery overwrote deferredValue, so this is the last
        // write of the burst.
        const double value =
                bitsToDouble(pEntry->deferredValueBits.loadAcquire());
        const double baseline = bitsToDouble(
                pEntry->deferredLeaderAtScheduleBits.loadAcquire());
        const double leaderNow = pEntry->pLeader->get();
        // Quiescence by equality: a relayed gesture already produced this
        // value on the leader through its own engine, so the echo of that
        // gesture is dropped and never doubles (the 9.12 a guarantee, now
        // enforced by ordering rather than by not relaying at all).
        if (leaderNow == value) {
            continue;
        }
        // Stale against a newer intent: the leader moved since the value
        // was scheduled, so something fresher (a leader-side command or a
        // mirrored gesture) owns the size now. Drop; the leader's result
        // mirrors back and the follower converges on it.
        if (leaderNow != baseline) {
            continue;
        }
        const BridgeWriteGuard writeGuard;
        pEntry->pLeader->set(value);
    }
}

void DeckPairManager::slotBridgeMirror(
        Pair* pPair, BridgeEntry* pEntry, double value, quint64 generation) {
    // Same staleness rule as slotBridgeRelay, and this is the side that needs
    // it: the mirror listener is an auto connection, so a leader write from the
    // engine thread arrives here as a queued invocation carrying raw pointers
    // that the teardown may have freed in the meantime.
    if (!pPair->active || pPair->bridgeGeneration != generation) {
        return;
    }
    // Delivered on the main thread: same-thread emissions are direct,
    // engine-thread emissions are queued by the auto connection.
    if (!pPair->active) {
        return;
    }
    // Paired members hold the same track (5.7). Cheap: two pointer compares.
    checkSameTrackInvariant(*pPair);
    // A slipping follower is not bridged at all: the relay side already
    // refuses follower commands while it slips (R8), and mirror writes must
    // stop too. A mirror is an external CO write, but a slipping follower
    // runs its live transport, which interprets such writes as commands: a
    // mirrored playposition lands as a seek on its own plane, a mirrored
    // loop status clobbers the deck-local roll or slice that slip engaged.
    // Bench finding (SC Live roll/slicer pads): the leader's 45 Hz
    // playposition mirror dragged the rolling follower back to the pair
    // plane on nearly every buffer -- "the effect begins and is countermanded
    // immediately" (DECK_PAIR_PLAN.md 9.11 m). On slip release the replica
    // re-anchors through refreshMirrorValues(), so nothing stays stale.
    if (pPair->pFollowerSlipEnabled && pPair->pFollowerSlipEnabled->toBool()) {
        return;
    }
    // The mirror is equally halted while the LEADER slips (bench round 6):
    // a rolling leader's loop status and playposition describe its
    // deck-local gesture, not the pair. Every buffer the leader's engine
    // publishes the audible, wrap-inside-the-roll position, and every roll
    // press rewrites the loop points/enabled with the roll's region --
    // mirrored, the follower engaged the roll region as a real loop and was
    // re-seeked into it at main-loop rate: "press a roll pad and the whole
    // track rolls" on both members, now on every press (the round-4 leader
    // gate made the roll engage reliably, which made this leak reliable).
    // The pair's truth needs no mirror while the leader slips: it rides the
    // transport lane, published from the leader's slip plane (5.2). On
    // release the replica re-anchors through refreshMirrorValues(), so
    // nothing stays stale (9.16; mirror of the 9.11 m rule above).
    if (pPair->pLeaderSlipEnabled && pPair->pLeaderSlipEnabled->toBool()) {
        return;
    }
    // Mid-roll loop adoption on the leader: same window logic as the relay
    // side (R8 / T30) -- slip_enabled is already cleared, but the engine has
    // not consumed the adoption yet, and the loop writes it produces are a
    // local act with zero pair effect.
    if (pPair->pLeaderBuffer != nullptr &&
            pPair->pLeaderBuffer->isSlipQuitAndAdoptPending()) {
        return;
    }
    // Freshness: a queued mirror emission can arrive after the leader has
    // already moved on (bench finding 9.11 l). Mirroring the stale value
    // would clobber fresher state on the follower, and since the echo of a
    // mirror is (by design, see slotBridgeRelay) never relayed back, nothing
    // would heal it. Drop the stale delivery instead: the newest queued
    // emission for this control carries the leader's current value.
    if (pEntry->pLeader->get() != value) {
        return;
    }
    if (pEntry->pFollower->get() == value) {
        return; // quiescence by equality, see slotBridgeRelay().
    }
    // The mirror's write is delivery, not command: mark it so its echo never
    // re-enters the relay (9.11 l).
    const BridgeWriteGuard writeGuard;
    if (pEntry->forced) {
        // Owner-only status control: there is no proxy write for these, the
        // engine writes them with forceSet and we do the same (never
        // touching the confirm protocol of ordinary controls).
        forceMirrorValue(activePairMemberGroup(pPair->followerDeckIndex),
                pEntry->item,
                value);
    } else {
        pEntry->pFollower->set(value);
    }
}

void DeckPairManager::forceMirrorValue(
        const QString& followerGroup, const QString& item, double value) {
    if (followerGroup.isEmpty()) {
        return;
    }
    ControlObject* pControl = ControlObject::getControl(
            ConfigKey(followerGroup, item));
    if (pControl != nullptr) {
        pControl->forceSet(value);
    }
}

QString DeckPairManager::activePairMemberGroup(
        int deckIndex) const {
    const Member* pMember = memberByIndex(deckIndex);
    return pMember ? pMember->group : QString();
}

void DeckPairManager::neutralizeFollowerAuthorities(Member& follower) {
    for (const QString& item : kRefusedFollowerAuthorityItems) {
        ControlObject* pControl = ControlObject::getControl(
                ConfigKey(follower.group, item));
        if (pControl != nullptr && pControl->get() != 0.0) {
            // Through the owner: the engine state behind it is unwound and
            // the confirmation snaps the control back to 0.
            pControl->set(0.0);
        }
    }
    for (const QString& item : kRefusedFollowerPlainItems) {
        ControlObject* pControl = ControlObject::getControl(
                ConfigKey(follower.group, item));
        if (pControl != nullptr && pControl->get() != 0.0) {
            pControl->set(0.0);
        }
    }
    // Whatever sync negotiation was in flight, the follower leaves it: a
    // follower-side tempo correction would double-correct against the pair
    // lock (5.8).
    follower.pEngineBuffer->requestSyncMode(SyncMode::None);
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
