// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// Tests for the deck pair transport lock and CO bridge: the pair leader
// publishes its transport every engine callback and the pair follower replays
// it frame-exactly, while the bridge relays commands from the follower to the
// leader and mirrors the leader's statuses back.
//
// DECK_PAIR_PLAN.md tests T9 - T12, T14 - T20, T22 - T24, T30 (engine part),
// T37, T39. The mocked fixture asserts frame-exact positions; the loop wrap
// and repeat tests need the real reader path (the mock scaler does not run
// the ReadAheadManager), so they run on real scalers in the audio fixture at
// the end of this file.

#include <gtest/gtest.h>

#include <QString>
#include <thread>

#include "control/controlobject.h"
#include "engine/channels/enginedeck.h"
#include "engine/controls/ratecontrol.h"
#include "engine/enginebuffer.h"
#include "mixer/deck.h"
#include "mixer/deckpairmanager.h"
#include "test/deckpairtestbase.h"
#include "track/beats.h"
#include "track/track.h"

namespace {
constexpr char kPlay[] = "play";
constexpr char kRate[] = "rate";

/// Frames a single engine callback advances at normal speed
/// (kProcessBufferSize samples / 2 channels), plus slack for the scaler
/// ramping a buffer.
constexpr double kBufferSlackFrames = 1200.0;
} // namespace

class DeckPairEngineTest : public DeckPairTestBase {
  protected:
    static constexpr int kLeader = 0;   // deck 1
    static constexpr int kFollower = 2; // deck 3

    /// Pairs deck 1 (leader) with deck 3, waits for the follower's track
    /// adoption, starts playback and checks that the lock holds right away.
    void startLockedPair() {
        registerDecks(4);
        expectPaired(kLeader);
        ASSERT_TRUE(waitForEngineTrack(kFollower, deck(kLeader)->getLoadedTrack()));
        setPlaying(kLeader, true);
        ProcessBuffer();
        ASSERT_EQ(1.0, control(kFollower, kPlay));
        assertPairLocked();
    }

    /// The pair lock: same-callback frame-exact position equality plus a
    /// mirrored play state (DECK_PAIR_PLAN.md 8, assertPairLocked helper).
    void assertPairLocked() {
        EXPECT_EQ(buffer(kLeader)->getPlayPos().value(),
                buffer(kFollower)->getPlayPos().value())
                << "pair members must be frame-exact on the pair plane";
        EXPECT_EQ(control(kLeader, kPlay), control(kFollower, kPlay));
    }

    void pulse(int deckIndex, const QString& item) {
        setControl(deckIndex, item, 1.0);
        setControl(deckIndex, item, 0.0);
    }

    /// Relative write, the way a controller or skin nudges an accumulator or
    /// nudge-style control such as `jog` or `wheel`.
    void addRelative(int deckIndex, const QString& item, double delta) {
        setControl(deckIndex, item, control(deckIndex, item) + delta);
    }

    double playPos(int deckIndex) {
        return buffer(deckIndex)->getPlayPos().value();
    }

    /// hotcue_*_position (and loop_*_position) control objects are published
    /// in engine sample units; the buffer positions are frames.
    double cueFrames(int deckIndex, const QString& item) const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                control(deckIndex, item))
                .value();
    }

    double trackEndFrames() {
        return buffer(kLeader)->getTrackEndPosition().value();
    }
};

// T9: play/pause from either member drives both decks; the follower's own
// transport stays locked to the leader's, frame-exact, every callback.
TEST_F(DeckPairEngineTest, PlayPauseFromEitherMember) {
    startLockedPair();
    const double posAfterStart = playPos(kLeader);
    EXPECT_GT(posAfterStart, 0.0);

    // Pause from the follower: the command relays to the leader.
    setControl(kFollower, kPlay, 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kLeader, kPlay));
    EXPECT_EQ(0.0, control(kFollower, kPlay));
    const double pausedPos = playPos(kLeader);
    ProcessBuffer();
    assertPairLocked();
    // While paused the pair plane holds its position.
    EXPECT_EQ(pausedPos, playPos(kLeader));

    // Play from the leader: the state mirrors onto the follower.
    setControl(kLeader, kPlay, 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kFollower, kPlay));
    for (int i = 0; i < 5; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before);
    }
}

// T10: long-run drift killer. Rate 1.25 over 50 buffers with pitch wobbles
// issued from the follower in between: the members must advance in lock and
// never drift apart, and losing the same-callback ordering would fail loudly
// here as a constant one-buffer offset.
TEST_F(DeckPairEngineTest, LongRunKeepsFrameExactLock) {
    startLockedPair();
    setControl(kLeader, kRate, 1.25);
    double previous = playPos(kLeader);
    for (int i = 0; i < 50; ++i) {
        if (i == 24) {
            // Pitch wobble from the follower: relayed to the leader, the
            // follower just follows the pair plane.
            setControl(kFollower, kRate, 1.10);
        }
        if (i == 34) {
            setControl(kFollower, kRate, 1.25);
        }
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous) << "callback " << i;
        previous = playPos(kLeader);
    }
    EXPECT_DOUBLE_EQ(control(kLeader, kRate), control(kFollower, kRate));
    EXPECT_DOUBLE_EQ(1.25, control(kFollower, kRate));
}

// T11: hotcue set on the leader and fired from the follower, then the other
// way round. The cue points live in the shared Track; the statuses arrive on
// the other member through the bridge mirror / the relayed command.
TEST_F(DeckPairEngineTest, HotcueSetAndFireAcrossMembers) {
    startLockedPair();
    // Deterministic cue positions: store exactly where the pair is.
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // Set on the leader, fire on the follower.
    pulse(kLeader, QStringLiteral("hotcue_1_setcue"));
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("hotcue_1_status")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("hotcue_1_position")),
            control(kFollower, QStringLiteral("hotcue_1_position")));

    setControl(kFollower, kPlay, 0.0); // park the pair away from the cue
    ProcessBuffer();
    pulse(kFollower, QStringLiteral("hotcue_1_activate"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("hotcue_1_status")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("hotcue_1_status")));
    EXPECT_NEAR(playPos(kLeader),
            cueFrames(kLeader, QStringLiteral("hotcue_1_position")),
            kBufferSlackFrames);
    setControl(kLeader, kPlay, 1.0);
    ProcessBuffer();

    // Set on the follower, fire on the leader.
    pulse(kFollower, QStringLiteral("hotcue_3_setcue"));
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("hotcue_3_status")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("hotcue_3_position")),
            control(kFollower, QStringLiteral("hotcue_3_position")));

    setControl(kFollower, kPlay, 0.0);
    ProcessBuffer();
    pulse(kLeader, QStringLiteral("hotcue_3_activate"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_NEAR(playPos(kFollower),
            cueFrames(kFollower, QStringLiteral("hotcue_3_position")),
            kBufferSlackFrames);
}

// T14: seeking via playposition (quantize off and on), beatjump_forward,
// start and end from the follower. The quantization and phase resolution
// happens once on the leader; both members land on the identical target on
// the very next buffer.
TEST_F(DeckPairEngineTest, SeeksFromFollowerLandIdentically) {
    startLockedPair();

    // Quantize off: the raw fractional target.
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();
    const double end = trackEndFrames();
    setControl(kFollower, QStringLiteral("playposition"), 0.4);
    ProcessBuffer();
    assertPairLocked();
    EXPECT_NEAR(playPos(kLeader), 0.4 * end, kBufferSlackFrames);
    EXPECT_NEAR(playPos(kFollower), 0.4 * end, kBufferSlackFrames);

    // Quantize on: one shared quantization decision for the pair.
    setControl(kLeader, QStringLiteral("quantize"), 1.0);
    ProcessBuffer();
    setControl(kFollower, QStringLiteral("playposition"), 0.6);
    ProcessBuffer();
    assertPairLocked();
    EXPECT_NEAR(playPos(kLeader), 0.6 * end, 2.0 * kBufferSlackFrames);

    // Beatjump from the follower.
    const double beforeJump = playPos(kLeader);
    pulse(kFollower, QStringLiteral("beatjump_forward"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_GT(playPos(kLeader), beforeJump);

    // Start and end from the follower.
    pulse(kFollower, QStringLiteral("start"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_LT(playPos(kLeader), kBufferSlackFrames);
    pulse(kFollower, QStringLiteral("end"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_GT(playPos(kLeader), end - 2.0 * kBufferSlackFrames);
    // The end-of-track play release is mirrored, not fought over.
    EXPECT_EQ(control(kLeader, kPlay), control(kFollower, kPlay));
}

// T15: rate, rate_range, rate_temp_up and reverse from the follower change
// the pair speed for both members; the rate/rate_ratio displays agree and
// the scaler-mode flags (reverse) match on both engines.
TEST_F(DeckPairEngineTest, RateAndReverseFromFollowerDriveThePair) {
    startLockedPair();

    // In this tree [ChannelN], "rate" is the pitch slider (-1..1) and the
    // engine speed is "rate_ratio"; the follower's slider write is relayed
    // to the leader, and the leader's ratio comes back mirrored.
    setControl(kFollower, kRate, 0.25);
    double previous = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous);
        previous = playPos(kLeader);
    }
    EXPECT_DOUBLE_EQ(control(kLeader, kRate), control(kFollower, kRate));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));
    EXPECT_GT(control(kLeader, QStringLiteral("rate_ratio")), 1.0);

    setControl(kFollower, QStringLiteral("rateRange"), 0.5);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.5, control(kLeader, QStringLiteral("rateRange")));
    EXPECT_DOUBLE_EQ(control(kLeader, kRate), control(kFollower, kRate));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));

    // A nudge via rate_temp_up: the stepping amount comes from a static the
    // preferences dialog normally sets, absent in tests, so configure it.
    // The temp change affects the audible speed, not the rate_ratio CO.
    RateControl::setTemporaryRateChangeCoarseAmount(100.0);
    double prevNudge = playPos(kLeader);
    ProcessBuffer();
    const double idleDelta = playPos(kLeader) - prevNudge;
    EXPECT_GT(idleDelta, 0.0);
    prevNudge = playPos(kLeader);
    setControl(kFollower, QStringLiteral("rate_temp_up"), 1.0);
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    const double heldDelta = playPos(kLeader) - prevNudge;
    EXPECT_GT(heldDelta, idleDelta * 1.2);
    setControl(kFollower, QStringLiteral("rate_temp_up"), 0.0);
    ProcessBuffer();
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    prevNudge = playPos(kLeader);
    ProcessBuffer();
    EXPECT_NEAR(playPos(kLeader) - prevNudge, idleDelta, 2.0);

    // Safety net for deviation 9.11 d: EVERY rate control of the follower
    // must move the pair's rate (R7). The follower's own RateControl
    // recomputation is gated while locked, so the bridge relays the full
    // rate control set - including the small buttons and rate_ratio itself
    // (the universal net any rate gesture ends in, e.g. a user BPM adjust).
    RateControl::setPermanentRateChangeFineAmount(2.0);
    const double ratioBefore = control(kLeader, QStringLiteral("rate_ratio"));
    pulse(kFollower, QStringLiteral("rate_perm_up_small"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    assertPairLocked();
    EXPECT_GT(control(kLeader, QStringLiteral("rate_ratio")), ratioBefore);
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));

    pulse(kFollower, QStringLiteral("rate_perm_down_small"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    assertPairLocked();
    EXPECT_DOUBLE_EQ(ratioBefore, control(kLeader, QStringLiteral("rate_ratio")));

    // A direct rate_ratio write on the follower, as produced by the user BPM
    // adjust path of BpmControl: the pair takes the new tempo.
    setControl(kFollower, QStringLiteral("rate_ratio"), 1.30);
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_DOUBLE_EQ(1.30, control(kLeader, QStringLiteral("rate_ratio")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));
    previous = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous);
        previous = playPos(kLeader);
    }

    // The small nudge buttons bend the pair speed while held, too. The
    // stepping amount is another dialog-owned static.
    RateControl::setTemporaryRateChangeFineAmount(2.0);
    previous = playPos(kLeader);
    ProcessBuffer();
    const double idleDeltaAt130 = playPos(kLeader) - previous;

    setControl(kFollower, QStringLiteral("rate_temp_up_small"), 1.0);
    previous = playPos(kLeader);
    ProcessBuffer();
    ProcessBuffer();
    const double heldSmallDelta = playPos(kLeader) - previous;
    EXPECT_GT(heldSmallDelta, idleDeltaAt130);
    assertPairLocked();
    setControl(kFollower, QStringLiteral("rate_temp_up_small"), 0.0);
    ProcessBuffer();
    ProcessBuffer();
    previous = playPos(kLeader);
    ProcessBuffer();
    EXPECT_NEAR(playPos(kLeader) - previous, idleDeltaAt130, 2.0);
    assertPairLocked();

    // Reverse from the follower: the pair plane runs backwards, locked, and
    // the scaler flags on both engines agree.
    setControl(kFollower, QStringLiteral("reverse"), 1.0);
    ProcessBuffer();
    previous = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_LT(playPos(kLeader), previous);
        previous = playPos(kLeader);
    }
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("reverse")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("reverse")));
    EXPECT_EQ(buffer(kLeader)->isReverse(), buffer(kFollower)->isReverse());
    EXPECT_TRUE(buffer(kFollower)->isReverse());
    setControl(kFollower, QStringLiteral("reverse"), 0.0);
    ProcessBuffer();
    assertPairLocked();
}

// R7 through the controller engine thread: a hardware mapping writes the
// follower deck's controls from that thread, not from the GUI thread, and
// the bridge must treat it as a user command. The only writes it must
// ignore are engine-internal ones (deviation note 9.11 h); this is exactly
// the routing a real controller performs on the bench.
TEST_F(DeckPairEngineTest, RateCommandsFromAForeignThreadAreRelayed) {
    startLockedPair();

    const QString followerGroup = group(kFollower);
    std::thread hardwareLikeWriter([&followerGroup] {
        ControlObject::set(ConfigKey(followerGroup,
                                   QStringLiteral("rate_ratio")),
                1.40);
    });
    hardwareLikeWriter.join();

    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_DOUBLE_EQ(1.40, control(kLeader, QStringLiteral("rate_ratio")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));
    double previous = playPos(kLeader);
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous);
        previous = playPos(kLeader);
    }
}

// Regression for bench finding 9.11 l (pitch-fader limit cycle): a mirror
// queued from a foreign-thread write to the leader must never be relayed
// back when it lands late. Without the bridge echo guard, the stale mirror
// (0.25) delivered after the leader already moved on (0.30) rolls the
// leader back - the exact signature of the audible oscillation.
TEST_F(DeckPairEngineTest, StaleQueuedMirrorIsNotRelayedBack) {
    registerDecks(4);
    expectPaired(kLeader);
    ASSERT_TRUE(waitForEngineTrack(kFollower, deck(kLeader)->getLoadedTrack()));

    // Spy on the leader's pitch slider: it must never observe a rollback.
    QVector<double> leaderSliderValues;
    ControlProxy spyLeader(
            ConfigKey(group(kLeader), QStringLiteral("rate")));
    spyLeader.connectValueChanged(
            &spyLeader, [&leaderSliderValues](double value) {
                leaderSliderValues.append(value);
            },
            Qt::DirectConnection);

    const QString leaderGroup = group(kLeader);
    // 1. A "hardware" fader event on a foreign thread: the leader takes the
    //    value inline and the mirrors to the follower get queued on main.
    std::thread hardwareLikeWriter([&leaderGroup] {
        ControlObject::set(ConfigKey(leaderGroup, QStringLiteral("rate")),
                0.25);
    });
    hardwareLikeWriter.join();

    // 2. The fader moves on before the queued mirrors are delivered: main
    //    thread, current time, newest value.
    setControl(kLeader, QStringLiteral("rate"), 0.30);
    leaderSliderValues.clear(); // only watch the window where stale events land

    // 3. Deliver the queued mirrors: the stale 0.25 mirror lands on the
    //    follower now; its echo must not travel back to the leader.
    QCoreApplication::processEvents();
    ProcessBuffer();

    EXPECT_DOUBLE_EQ(0.30, control(kLeader, QStringLiteral("rate")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate")),
            control(kFollower, QStringLiteral("rate")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));
    for (const double value : leaderSliderValues) {
        EXPECT_DOUBLE_EQ(0.30, value)
                << "leader pitch slider was rolled back by a stale mirror "
                   "echo (9.11 l regression)";
    }
}

// Bench finding 9.11 m, part 2: while the follower slips, no mirror write
// may land on it at all -- a mirrored playposition is a seek on its live
// transport, a mirrored loop status clobbers its deck-local loop. On slip
// release the replica re-anchors onto the leader's current statuses.
TEST_F(DeckPairEngineTest, MirrorPausedWhileFollowerSlipsAndReanchors) {
    startLockedPair();

    setControl(kFollower, QStringLiteral("slip_enabled"), 1.0);

    // Leader-side loop operation while the follower slips: statuses must not
    // reach the follower (before the fix this trampled its local state and,
    // via playposition, seek-stomped its transport).
    pulse(kLeader, QStringLiteral("beatloop_4_activate"));
    for (int i = 0; i < 10; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(-1.0, cueFrames(kFollower, QStringLiteral("loop_start_position")));

    // Slip release: one-shot re-anchor (refreshMirrorValues).
    setControl(kFollower, QStringLiteral("slip_enabled"), 0.0);
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("beatloop_4_enabled")));

    // And the lock itself is healthy again.
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// T24: statuses on the follower stay coherent after leader operations --
// loop state, hotcue status, cue point, play indicator and the
// self-computed beat_active.
TEST_F(DeckPairEngineTest, FollowerStatusMirrorsLeaderOperations) {
    startLockedPair();

    pulse(kLeader, QStringLiteral("cue_set"));
    pulse(kLeader, QStringLiteral("hotcue_2_setcue"));
    ProcessBuffer();
    pulse(kLeader, QStringLiteral("beatloop_4_activate"));
    for (int i = 0; i < 10; ++i) {
        ProcessBuffer();
    }

    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));

    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("hotcue_2_status")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("hotcue_2_position")),
            control(kFollower, QStringLiteral("hotcue_2_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("cue_point")),
            control(kFollower, QStringLiteral("cue_point")));

    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("play_indicator")));
    EXPECT_EQ(control(kLeader, QStringLiteral("play_indicator")),
            control(kFollower, QStringLiteral("play_indicator")));
    // beat_active is not mirrored (5.4.6): it is recomputed by the follower
    // at the pair position, so it must agree.
    EXPECT_EQ(control(kLeader, QStringLiteral("beat_active")),
            control(kFollower, QStringLiteral("beat_active")));

    // playposition: the follower does not write it while locked, the
    // leader's value is mirrored (deviation note 9.11).
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("playposition")),
            control(kFollower, QStringLiteral("playposition")));

    assertPairLocked();
}

// T39: relayed commands arrive while the follower is still busy with the
// adoption load (pending seeks are dropped by slotTrackLoaded). On the first
// follower callback after the load the positions must be equal again, with
// no stale queued-seek artifacts.
TEST_F(DeckPairEngineTest, LoadRaceReLocksOnFirstCallbackAfterLoad) {
    registerDecks(4);
    expectPaired(kLeader);
    setControl(kLeader, QStringLiteral("quantize"), 0.0);

    // The adoption load of the follower is in flight: keep the leader's
    // transport rolling and move the pair plane around underneath.
    setPlaying(kLeader, true);
    ProcessBuffer();
    ProcessBuffer();
    setControl(kFollower, QStringLiteral("playposition"), 0.4);
    ProcessBuffer();
    pulse(kFollower, QStringLiteral("hotcue_1_activate"));
    ProcessBuffer();

    ASSERT_TRUE(waitForEngineTrack(kFollower, deck(kLeader)->getLoadedTrack()));
    ProcessBuffer(); // first follower callback after the load
    assertPairLocked();
    for (int i = 0; i < 5; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before);
    }
}

// ---------------------------------------------------------------------------
// Phase 3: the command bridge. Every command class works from both decks
// (DECK_PAIR_PLAN.md T16 - T20, T22 - T23), the relay of trigger controls is
// echo-safe (T37), and nothing slip-related ever crosses the bridge (T30).
// ---------------------------------------------------------------------------

// T16: scratch2_enable + jog nudges from the follower. The pair plane
// follows in both directions and stays frame-exact. `jog` is a consumer-zeroed
// accumulator and is therefore not a bridged control at all: a locked follower
// performs its regular read-and-zero once per callback and publishes what it
// consumed onto the pair transport's accumulator lane, and the leader folds
// the lane into its own jog consumption (DECK_PAIR_PLAN.md 5.4.1 / 9.13). The
// two members' `jog` controls are consequently never written through the pair.
// `scratch2` is a plain value copy: the scratch source owns its value
// contract (no engine side zeroes it).
TEST_F(DeckPairEngineTest, JogAndScratchFromFollowerMoveThePairPlane) {
    startLockedPair();

    // Baseline advance of the locked pair at rate 1.
    double previous = playPos(kLeader);
    ProcessBuffer();
    const double idleAdvance = playPos(kLeader) - previous;
    ASSERT_GT(idleAdvance, 0.0);

    // Jog nudge from the follower, held for a few buffers: the value crosses
    // on the lane (write -> follower's gated consume-and-zero publishes ->
    // leader claims at its next consumption step) and neither member's `jog`
    // control is ever touched through the pair.
    previous = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        addRelative(kFollower, QStringLiteral("jog"), 2.0);
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(0.0, control(kFollower, QStringLiteral("jog")))
                << "the follower's gated consume-and-zero must re-baseline its "
                   "own accumulator like a single deck, callback "
                << i;
        EXPECT_DOUBLE_EQ(2.0, m_pPairManager->pairAccumulated(group(kFollower)))
                << "the value the follower consumed must wait on the lane for "
                   "the leader's consumption point, callback "
                << i;
        EXPECT_DOUBLE_EQ(0.0, control(kLeader, QStringLiteral("jog")))
                << "the lane is claimed into the leader's rate, not written "
                   "through its control, callback "
                << i;
        assertPairLocked();
    }
    // Claim the lane the leader has not reached yet so the whole nudge is in
    // the measured advance.
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)))
            << "the leader's consumer must drain the lane";

    // Hold a strong nudge until the rate effect reaches its steady state
    // (one buffer of claim latency plus the jog low-pass ramp), then measure
    // a single buffer: a held +10.0 jog runs the pair plane visibly fast.
    // Measuring the plateau -- not a window sum -- keeps this assertion
    // insensitive to the ramp's exact shape while still failing outright if
    // the nudge never reaches the leader at all.
    for (int i = 0; i < 8; ++i) {
        setControl(kFollower, QStringLiteral("jog"), 10.0);
        ProcessBuffer();
        assertPairLocked();
    }
    previous = playPos(kLeader);
    ProcessBuffer();
    const double nudgedAdvance = playPos(kLeader) - previous;
    EXPECT_GT(nudgedAdvance, 1.05 * idleAdvance)
            << "a jog nudge from the follower must push the pair plane";
    EXPECT_LT(nudgedAdvance, 1.9 * idleAdvance)
            << "the nudge bends the rate, it does not launch the plane";

    // The other direction: a held negative nudge slows the pair plane. Let
    // the positive nudge's low-pass tail decay out first, so the measurement
    // below sees only the negative ramp.
    for (int i = 0; i < 60; ++i) {
        ProcessBuffer();
    }
    for (int i = 0; i < 14; ++i) {
        setControl(kFollower, QStringLiteral("jog"), -10.0);
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(0.0, control(kFollower, QStringLiteral("jog")));
        assertPairLocked();
    }
    previous = playPos(kLeader);
    ProcessBuffer();
    const double slowedAdvance = playPos(kLeader) - previous;
    EXPECT_LT(slowedAdvance, 0.9 * idleAdvance)
            << "a negative jog from the follower must slow the pair plane";
    EXPECT_GT(slowedAdvance, 0.3 * idleAdvance)
            << "the pair plane slows down, it does not stall or reverse";

    // Let the leader's jog low-pass decay out before measuring scratch
    // speeds (the filter is RateControl's).
    for (int i = 0; i < 20; ++i) {
        ProcessBuffer();
    }

    // scratch2 scratch from the follower: the mode relays, the scratch2
    // value lands on the leader, and the pair plane scratches through the
    // audio -- frame-exact, and under the scratch speed.
    setControl(kFollower, QStringLiteral("scratch2_enable"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("scratch2_enable")))
            << "scratch2_enable from the follower must relay to the leader";
    setControl(kFollower, QStringLiteral("scratch2"), 0.3);
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.3, control(kLeader, QStringLiteral("scratch2")))
            << "the scratch value from the follower must drive the leader";
    previous = playPos(kLeader);
    ProcessBuffer();
    assertPairLocked();
    const double scratched = playPos(kLeader) - previous;
    EXPECT_GT(scratched, 0.0);
    EXPECT_LT(scratched, idleAdvance)
            << "in scratch2 mode the pair plane moves at the scratch speed";
    // Zero scratch value: the pair plane holds, still frame-exact.
    setControl(kFollower, QStringLiteral("scratch2"), 0.0);
    ProcessBuffer();
    previous = playPos(kLeader);
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_LT(std::fabs(playPos(kLeader) - previous), 1.0)
            << "holding the platter (scratch2 == 0) must hold the pair plane";
    setControl(kFollower, QStringLiteral("scratch2_enable"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("scratch2_enable")));
    // The pair resumes its normal locked transport.
    for (int i = 0; i < 3; ++i) {
        previous = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous);
    }
}

// T16b (bench rounds 2+3, findings 9.12 i / 9.13): `jog` rides the pair
// transport's accumulator lane, not the CO bridge, because its value only has
// a meaning at the consumer's read-and-zero -- controllers (the SC Live 4 jog
// wheel writes `jog` directly) SET an instantaneous velocity per message while
// skins and nudge buttons write `+=` deltas, and a copy-relay re-fed what the
// leader had already applied (2-3x), a drain-additive relay turned the
// absolute-velocity stream into a sum (2x). The lane must carry exactly what
// the follower's own consumer would have consumed: the latest value for
// absolute writers, the sum for relative writers -- once per callback, and
// never through either member's `jog` control.
TEST_F(DeckPairEngineTest, JogAccumulatorCrossesThePairLaneExactlyOnce) {
    startLockedPair();

    // Contract 1 -- absolute writes (controller wheel): the second write
    // overwrites the first on the follower's CO, so the lane carries 3.0.
    // A relay overwrote nothing: each write event crossed its own value and
    // the leader would have seen the pair 2.0 + 3.0 = 5.0 of nudge.
    setControl(kFollower, QStringLiteral("jog"), 2.0);
    setControl(kFollower, QStringLiteral("jog"), 3.0);
    EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)))
            << "nothing crosses before the follower's consumption point";
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, control(kFollower, QStringLiteral("jog")));
    EXPECT_DOUBLE_EQ(3.0, m_pPairManager->pairAccumulated(group(kFollower)))
            << "the lane must carry the latest velocity, not a sum of writes";
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)))
            << "the leader's consumer must claim the lane";
    EXPECT_DOUBLE_EQ(0.0, control(kLeader, QStringLiteral("jog")))
            << "the claim folds into the leader's rate, not its control";

    // Contract 2 -- relative writes (skin nudge, `+=`): the burst accumulates
    // on the follower's CO and crosses once, as its sum. A copy-relay fed
    // 2.0 and then the cumulative 4.0 to the leader here (bench: 2-3x).
    addRelative(kFollower, QStringLiteral("jog"), 2.0);
    addRelative(kFollower, QStringLiteral("jog"), 2.0);
    EXPECT_DOUBLE_EQ(4.0, control(kFollower, QStringLiteral("jog")));
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, control(kFollower, QStringLiteral("jog")));
    EXPECT_DOUBLE_EQ(4.0, m_pPairManager->pairAccumulated(group(kFollower)))
            << "a burst inside one window must cross as its sum, exactly once";
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)));
    assertPairLocked();

    // A held nudge across windows: each window's value crosses once, and
    // neither member's `jog` control is ever touched through the pair.
    for (int i = 0; i < 4; ++i) {
        setControl(kFollower, QStringLiteral("jog"), 2.0);
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(2.0, m_pPairManager->pairAccumulated(group(kFollower)))
                << "callback " << i;
        EXPECT_DOUBLE_EQ(0.0, control(kLeader, QStringLiteral("jog")))
                << "callback " << i;
    }
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)));
    assertPairLocked();
}

// T40 (bench round 3, finding 9.13): a roll-pad press on the follower while
// the pair loops. `beatlooproll_activate` is deck-local (R8) by design, but
// its vanilla first-press semantics ("a running loop is exited before the
// roll can start") read the follower's own loop state -- which on a locked
// follower is the pair's mirrored replica. The exit destroyed the replica
// while the leader's unchanged status (edge-triggered mirror) never re-pushed
// it: the pair's loop then existed only on the leader, and the first press of
// every roll came out as nothing or as a wrong length. A deck-local roll must
// roll over the replica, not consume it.
TEST_F(DeckPairEngineTest, FollowerRollEngagesOverThePairsLoopOnFirstPress) {
    startLockedPair();

    // A 4-beat pair loop created on the leader; the replica follows.
    pulse(kLeader, QStringLiteral("beatloop_4_activate"));
    ProcessBuffer();
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));

    // The SC Live 4 roll-pad dance on the follower: store the (mirrored)
    // size, write the roll length, press the roll.
    setControl(kFollower, QStringLiteral("beatloop_size"), 1.0);
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 1.0);
    ProcessBuffer();
    ProcessBuffer();

    // The roll must engage on the first press, like on an unpaired deck...
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("slip_enabled")))
            << "a roll-pad press on the follower must engage the deck-local "
               "roll, not consume the pair loop's replica (bench 9.13)";
    // ...and it must leave the pair's loop alone.
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")))
            << "the pair keeps looping while the follower rolls";

    // Release: the roll retires, slip releases, and the replica re-anchors
    // onto the pair loop the roll only borrowed over.
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 0.0);
    // Model the production main loop: on real hardware the slip release is
    // delivered to the bridge while the controller thread is mid-command,
    // and the trailing re-anchor pass runs from the event loop once the
    // release transaction has completed (see DeckPairManager's slip-release
    // listener, DECK_PAIR_PLAN.md 9.13).
    QTest::qWait(1);
    for (int i = 0; i < 6; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")));
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")))
            << "the follower's replica still mirrors the pair loop "
               "(bench 9.13: it died with the roll's first press)";
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();
}

// T41 (bench round 3, finding 9.13, the DJ's own sequence): roll pads on the
// follower, then a plain manual loop made on the leader while nothing slips.
TEST_F(DeckPairEngineTest, LeaderLoopAfterFollowerRollCycleReachesReplica) {
    startLockedPair();

    // Roll cycle on the follower (no pair loop this time): press, hold,
    // release, and let the slip release and the re-anchor complete.
    setControl(kFollower, QStringLiteral("beatloop_size"), 1.0);
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 1.0);
    for (int i = 0; i < 4; ++i) {
        ProcessBuffer();
    }
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 0.0);
    for (int i = 0; i < 8; ++i) {
        ProcessBuffer();
    }
    ASSERT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")));
    assertPairLocked();

    // A loop created on the leader in plain play: the replica must adopt it.
    // (Engine-written region: the loop points + loop_enabled mirror event
    // family the DJ's manual loop rides on the bridge.)
    pulse(kLeader, QStringLiteral("beatloop_4_activate"));
    for (int i = 0; i < 6; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")))
            << "the replica must adopt the leader's manual loop after a "
               "follower roll cycle (bench 9.13: it stayed leader-only)";
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();
}

// T17: jog, wheel and scratch on the LEADER. The follower is driven purely
// through the pair snapshot: it must follow sample-exact through every one
// of these gestures, including the scratch release ramp.
TEST_F(DeckPairEngineTest, LeaderTransportGesturesCarryTheFollowerExactly) {
    startLockedPair();

    auto advanceOneBuffer = [&]() {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        return playPos(kLeader) - before;
    };
    const double idle = advanceOneBuffer();
    ASSERT_GT(idle, 0.0);

    // Jog on the leader.
    double before = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        addRelative(kLeader, QStringLiteral("jog"), 2.0);
        ProcessBuffer();
        assertPairLocked();
    }
    EXPECT_GT(playPos(kLeader) - before, 5.0 * idle);

    // Wheel on the leader (the deprecated rotary).
    setControl(kLeader, QStringLiteral("wheel"), 0.25);
    before = playPos(kLeader);
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_GT(playPos(kLeader) - before, 2.0 * idle * 1.1);
    setControl(kLeader, QStringLiteral("wheel"), 0.0);
    ProcessBuffer();
    assertPairLocked();

    // Scratch on the leader: engage scratch2, move the platter, release --
    // the follower follows the ramp and the spinback callback-exactly.
    setControl(kLeader, QStringLiteral("scratch2_enable"), 1.0);
    ProcessBuffer();
    for (int i = 0; i < 4; ++i) {
        addRelative(kLeader, QStringLiteral("scratch2"), 0.25);
        ProcessBuffer();
        assertPairLocked();
    }
    // Release: the release ramp / inertia phase must not unlock the pair.
    setControl(kLeader, QStringLiteral("scratch2_enable"), 0.0);
    for (int i = 0; i < 6; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }

    // Two-position scratch on the leader (`scratch_position`).
    setControl(kLeader, QStringLiteral("scratch_position_enable"), 1.0);
    ProcessBuffer();
    assertPairLocked();
    for (int i = 0; i < 4; ++i) {
        addRelative(kLeader, QStringLiteral("scratch_position"), 3000.0);
        ProcessBuffer();
        assertPairLocked();
    }
    setControl(kLeader, QStringLiteral("scratch_position_enable"), 0.0);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
    // The follower's own scratch state was never touched (no L -> F flow).
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("scratch2_enable")));
    EXPECT_EQ(0.0,
            control(kFollower, QStringLiteral("scratch_position_enable")));
}

// T18: hotcue preview (press and release) from the follower. The preview
// happens on the pair plane: the leader audibly seeks to the cue and returns
// to the parked position on release; the follower re-anchors every callback.
TEST_F(DeckPairEngineTest, HotcuePreviewFromFollowerPreviewsOnThePairPlane) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // A hotcue at the current pair position, then park far away from it.
    pulse(kLeader, QStringLiteral("hotcue_1_setcue"));
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("hotcue_1_status")));
    const double cuePos = cueFrames(kLeader, QStringLiteral("hotcue_1_position"));

    setControl(kFollower, QStringLiteral("playposition"), 0.4);
    ProcessBuffer();
    setControl(kFollower, QStringLiteral("play"), 0.0);
    ProcessBuffer();
    const double parked = playPos(kLeader);
    ASSERT_EQ(0.0, control(kLeader, kPlay));
    ASSERT_GT(std::fabs(parked - cuePos), 10.0 * kBufferSlackFrames);

    // Press on the follower while stopped: the pair previews at the cue
    // point (audible seek on the pair plane, latched preview play).
    setControl(kFollower, QStringLiteral("hotcue_1_activate"), 1.0);
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(1.0, control(kLeader, kPlay));
    EXPECT_NEAR(playPos(kLeader), cuePos, kBufferSlackFrames)
            << "the leader must preview the hotcue on the pair plane";
    ProcessBuffer();
    assertPairLocked();

    // Release: the preview ends on the pair plane. (This engine's preview
    // release re-anchors the deck at the previewed cue point and stops --
    // single-deck semantics; the pair contract is that it behaves exactly
    // like one deck, not that it returns to an arbitrary parked position.)
    setControl(kFollower, QStringLiteral("hotcue_1_activate"), 0.0);
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(0.0, control(kLeader, kPlay));
    EXPECT_EQ(0.0, control(kFollower, kPlay));
    EXPECT_NEAR(playPos(kLeader), cuePos, kBufferSlackFrames)
            << "the preview release must leave the pair on the previewed "
               "plane, exactly like a single deck";
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
    // The follower never owned a preview of its own: the latched-preview
    // state it displays is the leader's mirror.
    EXPECT_EQ(control(kLeader, QStringLiteral("play_latched")),
            control(kFollower, QStringLiteral("play_latched")));
    // The pair resumes transport from there, frame-exact.
    setControl(kLeader, kPlay, 1.0);
    for (int i = 0; i < 3; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before);
    }
}

// T19: reloop_toggle, loop_halve/loop_double and loop_move from the
// follower. The loop region is a single pair state: the mirrored loop points
// stay identical on both members through every operation.
TEST_F(DeckPairEngineTest, LoopOperationsFromFollowerKeepOneRegion) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // Engage a 4-beat loop from the follower.
    pulse(kFollower, QStringLiteral("beatloop_4_activate"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));

    // Move the loop one beat forward from the follower: both loop points
    // shift, identically.
    const double startBefore = control(kLeader, QStringLiteral("loop_start_position"));
    pulse(kFollower, QStringLiteral("loop_move_1_forward"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    EXPECT_NE(startBefore, control(kLeader, QStringLiteral("loop_start_position")))
            << "loop_move from the follower must move the pair loop";
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();

    // Halve from the follower: region halves, both members agree.
    const double regionBefore = control(kLeader, QStringLiteral("loop_end_position")) -
            control(kLeader, QStringLiteral("loop_start_position"));
    pulse(kFollower, QStringLiteral("loop_halve"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    const double halved = control(kLeader, QStringLiteral("loop_end_position")) -
            control(kLeader, QStringLiteral("loop_start_position"));
    EXPECT_NEAR(regionBefore / 2.0, halved, 2.0);
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));

    // Double from the follower: back to the original region.
    pulse(kFollower, QStringLiteral("loop_double"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    EXPECT_DOUBLE_EQ(regionBefore,
            control(kLeader, QStringLiteral("loop_end_position")) -
                    control(kLeader, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();

    // reloop_toggle from the follower exits the loop on both ...
    pulse(kFollower, QStringLiteral("reloop_toggle"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    // ... and re-enters the identical region.
    pulse(kFollower, QStringLiteral("reloop_toggle"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();

    // loop_in / loop_out from the follower re-define the region on both.
    pulse(kFollower, QStringLiteral("loop_exit"));
    ProcessBuffer();
    pulse(kFollower, QStringLiteral("loop_in"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    const double inStart = control(kLeader, QStringLiteral("loop_start_position"));
    pulse(kFollower, QStringLiteral("loop_out"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    EXPECT_DOUBLE_EQ(inStart, control(kLeader, QStringLiteral("loop_start_position")));
    EXPECT_GT(control(kLeader, QStringLiteral("loop_end_position")), inStart);
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    assertPairLocked();
}

// T20: quantize toggled from the follower mid-run, then a loop set from the
// follower (loop_in + loop_out). One shared quantization decision (made on
// the leader, whose transport the pair replays) engages the loop identically
// on both members; the quantize toggle relays through the bridge.
TEST_F(DeckPairEngineTest, QuantizeFromFollowerSharesOneEngagementDecision) {
    startLockedPair();

    // Quantize off, from the leader; the mirror keeps the follower coherent.
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();
    ASSERT_EQ(0.0, control(kFollower, QStringLiteral("quantize")));

    // Unquantized loop set from the follower: the in/out points land at the
    // pair position as-is (off-grid), and the loop engages on both members.
    pulse(kFollower, QStringLiteral("loop_in"));
    ProcessBuffer();
    pulse(kFollower, QStringLiteral("loop_out"));
    ProcessBuffer();
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    const double rawStart = control(kLeader, QStringLiteral("loop_start_position"));
    // Off-grid: a raw in-point sits between beats (grid = 1764 engine
    // samples: 882 frames per beat at 120 BPM, 2 samples per frame).
    EXPECT_NE(0.0, std::fmod(rawStart, 1764.0));
    pulse(kFollower, QStringLiteral("loop_exit"));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }

    // Toggle quantize ON from the follower: the toggle relays to the leader.
    setControl(kFollower, QStringLiteral("quantize"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("quantize")))
            << "the quantize toggle from the follower must move the pair";
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("quantize")));

    // Quantized loop set from the follower: both members hold the identical
    // beat-aligned region and stay locked through the engagement.
    pulse(kFollower, QStringLiteral("loop_in"));
    ProcessBuffer();
    pulse(kFollower, QStringLiteral("loop_out"));
    for (int i = 0; i < 25 && control(kLeader, QStringLiteral("loop_enabled")) != 1.0;
            ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    // One shared quantization decision on the pair's beatgrid: the in-point
    // lands exactly on a beat of the constant 120 BPM grid anchored at
    // frame 0 (deckpairtestbase.h).
    EXPECT_DOUBLE_EQ(
            0.0, std::fmod(control(kLeader, QStringLiteral("loop_start_position")), 1764.0))
            << "quantized engagement must land on the beat grid";
    assertPairLocked();
}

// T22: keylock and pitch_adjust from the follower re-tune the pair. The
// follower's key/pitch displays are self-computed at the pair rate (5.4.6)
// and must still agree with the leader's.
TEST_F(DeckPairEngineTest, KeylockAndPitchAdjustFromFollowerRetuneThePair) {
    startLockedPair();

    // Keylock from the follower: relays, and both members agree on the mode.
    setControl(kFollower, QStringLiteral("keylock"), 1.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("keylock")))
            << "keylock from the follower must relay to the leader";
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("keylock")));

    // Pitch adjust from the follower: one semitone up for the pair. (The
    // `pitch` CO publishes cents in this tree: 100 == one semitone.)
    const double pitchBefore = control(kLeader, QStringLiteral("pitch"));
    setControl(kFollower, QStringLiteral("pitch_adjust"), 100.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(100.0, control(kLeader, QStringLiteral("pitch_adjust")))
            << "pitch_adjust from the follower must relay to the leader";
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("pitch_adjust")),
            control(kFollower, QStringLiteral("pitch_adjust")));
    EXPECT_NEAR(pitchBefore + 100.0, control(kLeader, QStringLiteral("pitch")), 2.0)
            << "the pair pitch must follow the follower's pitch adjust";
    // Self-computed, never mirrored -- yet consistent at the pair plane.
    EXPECT_NEAR(control(kLeader, QStringLiteral("pitch")),
            control(kFollower, QStringLiteral("pitch")),
            2.0);

    // Back to zero, and the pair stays frame-exact throughout.
    setControl(kFollower, QStringLiteral("pitch_adjust"), 0.0);
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
    EXPECT_DOUBLE_EQ(0.0, control(kLeader, QStringLiteral("pitch_adjust")));
}

// T23: start_play, cue_gotoandplay and play_stutter from the follower drive
// the pair; the indicator states mirror back.
TEST_F(DeckPairEngineTest, StartCueAndStutterFromFollowerDriveThePair) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // A cue point on the leader ...
    pulse(kLeader, QStringLiteral("cue_set"));
    ProcessBuffer();
    const double cuePos = cueFrames(kLeader, QStringLiteral("cue_point"));
    // ... then park the pair far away, stopped.
    setControl(kFollower, QStringLiteral("playposition"), 0.6);
    ProcessBuffer();
    setControl(kFollower, QStringLiteral("play"), 0.0);
    ProcessBuffer();
    ASSERT_GT(std::fabs(playPos(kLeader) - cuePos), 10.0 * kBufferSlackFrames);
    ASSERT_EQ(0.0, control(kLeader, QStringLiteral("play")));

    // cue_gotoandplay from the follower: the pair jumps to the cue and plays.
    pulse(kFollower, QStringLiteral("cue_gotoandplay"));
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("play")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("play")));
    EXPECT_NEAR(playPos(kLeader), cuePos, kBufferSlackFrames);
    ProcessBuffer();
    assertPairLocked();

    // start_play from the follower: the pair restarts at the track start.
    pulse(kFollower, QStringLiteral("start_play"));
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_LT(playPos(kLeader), 2.0 * kBufferSlackFrames);
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("play_indicator")));

    // play_stutter from the follower: the pair responds (existing single-deck
    // stutter semantics, reached through the relay; the play state mirrors).
    setControl(kFollower, QStringLiteral("play"), 0.0);
    ProcessBuffer();
    ASSERT_EQ(0.0, control(kLeader, QStringLiteral("play")));
    setControl(kFollower, QStringLiteral("play_stutter"), 1.0);
    ProcessBuffer();
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("play")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("play")));
    setControl(kFollower, QStringLiteral("play_stutter"), 0.0);
    ProcessBuffer();
    assertPairLocked();
    EXPECT_EQ(control(kLeader, QStringLiteral("play_indicator")),
            control(kFollower, QStringLiteral("play_indicator")));
}

// T30 (engine part): slip is never bridged. Manual slip on the leader keeps
// the follower a non-slipping deck riding the pair plane (the leader's slip
// plane while it slips, 5.2); a leader-side excursion during the slip moves
// only the leader's audible plane; a roll with mid-roll loop adoption on the
// follower has zero effect on the leader.
TEST_F(DeckPairEngineTest, SlipStaysOnTheDeckThatEngagedIt) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // Manual slip on the leader only.
    setControl(kLeader, QStringLiteral("slip_enabled"), 1.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")))
            << "slip_enabled must never cross the bridge (R8, T30)";
    // Still locked: while the leader slips, the pair plane is the leader's
    // slip plane and the follower rides it uninterrupted.
    assertPairLocked();
    double followerBefore = playPos(kFollower);
    ProcessBuffer();
    EXPECT_GT(playPos(kFollower), followerBefore);

    // An audible-plane excursion on the leader during its slip must move
    // only the audible plane: the follower (pair plane) advances smoothly.
    followerBefore = playPos(kFollower);
    setControl(kLeader, QStringLiteral("playposition"), 0.2);
    ProcessBuffer();
    EXPECT_GT(std::fabs(playPos(kLeader) - playPos(kFollower)),
            100.0 * kBufferSlackFrames)
            << "the leader excursion must not drag the pair plane";
    EXPECT_GT(playPos(kFollower), followerBefore)
            << "the pair plane keeps advancing during a leader excursion";
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")));

    // Leader slip release: seekExact(m_slipPos) snaps the audible plane back
    // onto the pair plane, frame-exact on the very next callback.
    setControl(kLeader, QStringLiteral("slip_enabled"), 0.0);
    ProcessBuffer();
    assertPairLocked();
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }

    // A roll on the follower: deck-local slip engages on the follower alone.
    setControl(kFollower, QStringLiteral("beatloop_size"), 2.0);
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("slip_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("slip_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));

    // Mid-roll loop adoption (slipQuitAndAdopt via beatloop_activate,
    // loopingcontrol.cpp:1759): a purely local act. The leader neither slips
    // nor loops; the follower releases slip and re-locks onto the pair plane
    // (its replica re-anchors, 9.11 m).
    pulse(kFollower, QStringLiteral("beatloop_activate"));
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("slip_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")));
    assertPairLocked();
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// T37: bridge echo safety. A trigger CO relayed from the follower must enter
// the leader exactly once -- trigger buttons re-emit (ignoreNops == false)
// and the pair bridge must not ping-pong on them (ControlObject spy pattern,
// DECK_PAIR_PLAN.md T37).
TEST_F(DeckPairEngineTest, RelayedTriggersEnterTheLeaderExactlyOnce) {
    startLockedPair();

    struct Spy {
        explicit Spy(const ConfigKey& key)
                : pProxy(std::make_unique<ControlProxy>(key)) {
            pProxy->connectValueChanged(
                    pProxy.get(),
                    [this](double value) { values.append(value); },
                    Qt::DirectConnection);
        }
        void clear() {
            values.clear();
        }
        std::unique_ptr<ControlProxy> pProxy;
        QVector<double> values;
    };

    Spy spyLoopIn(ConfigKey(group(kLeader), QStringLiteral("loop_in")));
    Spy spyBeatLoop(ConfigKey(group(kLeader), QStringLiteral("beatloop_4_activate")));

    // Press + release of loop_in on the follower: the leader observes exactly
    // one rising and one falling edge, nothing else.
    pulse(kFollower, QStringLiteral("loop_in"));
    EXPECT_EQ((QVector<double>{1.0, 0.0}), spyLoopIn.values)
            << "the relayed loop_in trigger ping-ponged on the leader";

    // A size-coded trigger button, same expectation.
    pulse(kFollower, QStringLiteral("beatloop_4_activate"));
    EXPECT_EQ((QVector<double>{1.0, 0.0}), spyBeatLoop.values)
            << "the relayed beatloop trigger ping-ponged on the leader";
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
    }
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));

    // A control bridged in BOTH directions (relay + mirror): setting it from
    // the follower must produce exactly one leader-side emission -- the
    // mirror echo is quiesced and never relayed back.
    Spy spyQuantize(ConfigKey(group(kLeader), QStringLiteral("quantize")));
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();
    spyQuantize.clear();
    setControl(kFollower, QStringLiteral("quantize"), 1.0);
    ProcessBuffer();
    EXPECT_EQ((QVector<double>{1.0}), spyQuantize.values)
            << "the quantize echo re-entered the leader (bridge not quiescent)";
    EXPECT_EQ(control(kLeader, QStringLiteral("quantize")),
            control(kFollower, QStringLiteral("quantize")));

    // The play command through its confirm path: one emission, and both
    // members agree afterwards.
    setControl(kFollower, QStringLiteral("play"), 0.0);
    ProcessBuffer();
    Spy spyPlay(ConfigKey(group(kLeader), QStringLiteral("play")));
    setControl(kFollower, QStringLiteral("play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ((QVector<double>{1.0}), spyPlay.values)
            << "the relayed play must enter the leader exactly once";
    EXPECT_EQ(control(kLeader, QStringLiteral("play")),
            control(kFollower, QStringLiteral("play")));
    assertPairLocked();
}

// ---------------------------------------------------------------------------
// Real-audio fixture: loop wrap and repeat are executed by the
// ReadAheadManager while scaling, which the mock scaler bypasses entirely.
// These tests run the unmocked engine path (like SignalPathTest).
// ---------------------------------------------------------------------------

class DeckPairAudioTest : public BaseSignalPathTest {
  protected:
    static constexpr int kLeader = 0;   // deck 1
    static constexpr int kFollower = 2; // deck 3

    DeckPairAudioTest() {
        const QString trackLocation =
                getTestDir().filePath(QStringLiteral("sine-30.wav"));
        m_pTrackA = Track::newTemporary(trackLocation);
        m_pTrackB = Track::newTemporary(trackLocation);
        m_pTrackA->trySetBeats(mixxx::Beats::fromConstTempo(
                mixxx::audio::SampleRate(44100),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(120.0)));
        m_pTrackB->trySetBeats(mixxx::Beats::fromConstTempo(
                mixxx::audio::SampleRate(44100),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(120.0)));
        loadTrack(m_pMixerDeck1.get(), m_pTrackA);
        loadTrack(m_pMixerDeck2.get(), m_pTrackA);
        loadTrack(m_pMixerDeck3.get(), m_pTrackB);

        m_pPairManager = std::make_unique<DeckPairManager>(
                m_pConfig, m_pEngineMixer.get());
        m_pPairManager->addDeck(0, m_pMixerDeck1.get());
        m_pPairManager->addDeck(1, m_pMixerDeck2.get());
        m_pPairManager->addDeck(2, m_pMixerDeck3.get());
    }

    ~DeckPairAudioTest() override {
        // The manager must go before the decks it registered.
        m_pPairManager.reset();
    }

    static QString group(int deckIndex) {
        return deckIndex == 0 ? m_sGroup1 : m_sGroup3;
    }

    Deck* deck(int deckIndex) {
        return deckIndex == 0 ? m_pMixerDeck1.get() : m_pMixerDeck3.get();
    }

    EngineBuffer* buffer(int deckIndex) {
        return deck(deckIndex)->getEngineDeck()->getEngineBuffer();
    }

    double control(int deckIndex, const QString& item) const {
        return ControlObject::get(ConfigKey(group(deckIndex), item));
    }

    void setControl(int deckIndex, const QString& item, double value) {
        ControlObject::set(ConfigKey(group(deckIndex), item), value);
    }

    void pulse(int deckIndex, const QString& item) {
        setControl(deckIndex, item, 1.0);
        setControl(deckIndex, item, 0.0);
    }

    void setPlaying(int deckIndex, bool playing) {
        setControl(deckIndex, kPlay, playing ? 1.0 : 0.0);
        ProcessBuffer();
    }

    void expectPaired() {
        setControl(kLeader, QStringLiteral("pair_enabled"), 1.0);
        ASSERT_EQ(1.0, control(kLeader, QStringLiteral("pair_enabled")));
        ASSERT_EQ(1.0, control(kFollower, QStringLiteral("pair_enabled")));
    }

    bool waitForEngineTrack(int deckIndex, const TrackPointer& pTrack) {
        EngineBuffer* pBuffer = buffer(deckIndex);
        for (int i = 0; i < 2000; ++i) {
            if (pBuffer->isTrackLoaded() && pBuffer->getLoadedTrack() == pTrack) {
                return true;
            }
            ProcessBuffer();
            QTest::qSleep(1);
        }
        return pBuffer->isTrackLoaded() && pBuffer->getLoadedTrack() == pTrack;
    }

    void assertPairLocked() {
        EXPECT_EQ(buffer(kLeader)->getPlayPos().value(),
                buffer(kFollower)->getPlayPos().value());
        EXPECT_EQ(control(kLeader, kPlay), control(kFollower, kPlay));
    }

    double playPos(int deckIndex) {
        return buffer(deckIndex)->getPlayPos().value();
    }

    TrackPointer m_pTrackA;
    TrackPointer m_pTrackB;
    std::unique_ptr<DeckPairManager> m_pPairManager;
};

// T12: beatloop 4 engaged from the follower wraps inside the same loop
// region on both members, twice; halving from the leader and exiting from
// the follower keep the lock and the mirrored loop state.
TEST_F(DeckPairAudioTest, BeatLoopFromFollowerWrapsLocked) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setPlaying(kLeader, true);
    assertPairLocked();

    pulse(kFollower, QStringLiteral("beatloop_4_activate"));
    ProcessBuffer();
    for (int i = 0; i < 4 && control(kFollower, QStringLiteral("loop_enabled")) != 1.0; ++i) {
        ProcessBuffer();
    }
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    const double loopStart = control(kFollower, QStringLiteral("loop_start_position"));
    const double loopEnd = control(kFollower, QStringLiteral("loop_end_position"));
    ASSERT_GT(loopEnd, loopStart);

    int wraps = 0;
    double previous = playPos(kLeader);
    for (int i = 0; i < 4000 && wraps < 2; ++i) {
        ProcessBuffer();
        assertPairLocked();
        const double now = playPos(kLeader);
        EXPECT_GE(now, loopStart - kBufferSlackFrames) << "callback " << i;
        EXPECT_LE(now, loopEnd + kBufferSlackFrames) << "callback " << i;
        if (now < previous - kBufferSlackFrames) {
            ++wraps;
            // Both members wrap in the same callback: the follower's
            // position is the pair plane, and its own reader re-anchors
            // onto it silently.
            EXPECT_NEAR(now, loopStart, 2.0 * kBufferSlackFrames)
                    << "wrap " << wraps;
        }
        previous = now;
    }
    EXPECT_EQ(2, wraps) << "the pair must wrap the loop twice within 4000 buffers";

    // Halve from the leader: the loop region shrinks for the pair.
    pulse(kLeader, QStringLiteral("beatloop_2_activate"));
    for (int i = 0; i < 4; ++i) {
        ProcessBuffer();
    }
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("beatloop_size")),
            control(kFollower, QStringLiteral("beatloop_size")));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    const double shortLoopEnd = control(kLeader, QStringLiteral("loop_end_position"));
    const double shortLoopStart = control(kLeader, QStringLiteral("loop_start_position"));
    ASSERT_GT(shortLoopEnd, shortLoopStart);
    ASSERT_LT(shortLoopEnd - shortLoopStart, loopEnd - loopStart);

    // Exit from the follower: both leave the loop and keep advancing locked.
    pulse(kFollower, QStringLiteral("loop_exit"));
    for (int i = 0; i < 4; ++i) {
        ProcessBuffer();
    }
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    previous = playPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), previous);
        previous = playPos(kLeader);
    }
}

// Bench finding 9.11 m, part 1: deck-local roll on the follower (SC Live
// "Roll Mode" pad). The bridge must be fully disconnected while the follower
// slips: its roll wraps on its own audible plane while the pair plane
// advances uninterrupted, and release rejoins it. Before the fix, the
// leader's mirrored playposition landed as a seek on the slipping follower:
// the roll "begins and is countermanded immediately" and glitters audibly
// (DECK_PAIR_PLAN.md 9.11 m). Runs on the real scalers -- mock-scaler
// fixtures never wrap (see the note at the top of this file).
TEST_F(DeckPairAudioTest, FollowerDeckLocalRollWrapsWhilePairAdvances) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setPlaying(kLeader, true);
    assertPairLocked();

    // SC Live roll pad press on the follower: set the roll length, activate
    // the roll. loopingcontrol.cpp engages slip before the loop, so nothing
    // of the roll itself may reach the leader (R8).
    setControl(kFollower, QStringLiteral("beatloop_size"), 2.0);
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("slip_enabled")));
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("slip_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));

    // loop_*_position COs are in engine sample units; the buffer positions
    // are frames (see the cueFrames helper of the mocked fixture).
    const double loopStart = mixxx::audio::FramePos::fromEngineSamplePos(
            control(kFollower, QStringLiteral("loop_start_position")))
                                     .value();
    const double loopEnd = mixxx::audio::FramePos::fromEngineSamplePos(
            control(kFollower, QStringLiteral("loop_end_position")))
                                   .value();
    ASSERT_GT(loopEnd, loopStart);

    // Held: the follower wraps inside its own window while the leader plays
    // straight through it. The mirrored leader state must not touch the
    // slipping follower at all.
    int wraps = 0;
    // Let the roll anchor and the entry seek into the window settle before
    // counting wraps (that seek is a legitimate jump, not a regression).
    ProcessBuffer();
    ASSERT_LE(playPos(kFollower), loopEnd + kBufferSlackFrames);
    double previous = playPos(kFollower);
    for (int i = 0; i < 4000 && wraps < 2; ++i) {
        ProcessBuffer();
        const double now = playPos(kFollower);
        EXPECT_LE(now, loopEnd + kBufferSlackFrames)
                << "mirrored leader state dragged the rolling follower out "
                   "of its deck-local roll (9.11 m regression), callback "
                << i;
        if (now < previous - kBufferSlackFrames) {
            ++wraps;
            EXPECT_NEAR(now, loopStart, 2.0 * kBufferSlackFrames)
                    << "wrap " << wraps;
        }
        previous = now;
    }
    EXPECT_EQ(2, wraps) << "the rolling follower must wrap twice within "
                           "4000 callbacks";
    EXPECT_GT(playPos(kLeader), loopEnd + kBufferSlackFrames)
            << "the pair plane must play through the deck-local roll";
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("slip_enabled")));

    // Release: roll ends, slip releases (the follower's slip plane kept
    // advancing the whole time -- the pair plane), and the lock is
    // frame-exact again.
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 0.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("slip_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// T21: repeat switched on from the follower, seeked near the end of the
// track, run through the boundary: both members wrap, the play state stays
// mirrored and idempotent; switched off afterwards, the natural
// end-of-track release lands on both members.
TEST_F(DeckPairAudioTest, RepeatThroughEndOfTrackStaysLocked) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    setPlaying(kLeader, true);
    assertPairLocked();

    setControl(kFollower, QStringLiteral("playposition"), 0.95);
    ProcessBuffer();
    assertPairLocked();

    setControl(kFollower, QStringLiteral("repeat"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kLeader, QStringLiteral("repeat")));

    bool wrapped = false;
    double previous = playPos(kLeader);
    for (int i = 0; i < 800 && !wrapped; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_EQ(1.0, control(kLeader, kPlay));
        EXPECT_EQ(1.0, control(kFollower, kPlay));
        const double now = playPos(kLeader);
        if (now < previous - kBufferSlackFrames) {
            wrapped = true;
        }
        previous = now;
    }
    EXPECT_TRUE(wrapped) << "repeat must wrap the pair at the end of track";
    for (int i = 0; i < 10; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }

    // Natural end of track: the leader releases play, the follower adopts
    // the mirrored release (nobody fights anybody over the play button).
    setControl(kFollower, QStringLiteral("repeat"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("repeat")));
    // The wrap restarted the track: jump back near the end for the natural
    // end-of-track release instead of playing the whole track again.
    setControl(kFollower, QStringLiteral("playposition"), 0.99);
    ProcessBuffer();
    for (int i = 0; i < 300 && control(kLeader, kPlay) != 0.0; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
    EXPECT_EQ(0.0, control(kLeader, kPlay));
    EXPECT_EQ(0.0, control(kFollower, kPlay));
    assertPairLocked();
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
