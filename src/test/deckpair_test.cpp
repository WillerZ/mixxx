// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// Phase 2 tests for the deck pair transport lock: the pair leader publishes
// its transport every engine callback and the pair follower replays it
// frame-exactly, while the Phase 2 CO bridge relays transport commands from
// the follower to the leader and mirrors the leader's play state back.
//
// DECK_PAIR_PLAN.md tests T9 - T12, T14 - T15, T21, T24, T39. The mocked
// fixture asserts frame-exact positions; the loop wrap and repeat tests need
// the real reader path (the mock scaler does not run the ReadAheadManager),
// so they run on real scalers in the audio fixture at the end of this file.

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
