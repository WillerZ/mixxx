// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// Tests for the deck pair slip semantics (DECK_PAIR_PLAN.md Phase 4):
// slip is never coupled (R8), and a slipping pair follower keeps its slip
// plane -- the position it snaps back to -- locked to the pair plane through
// the §5.4.3 feed, so releases rejoin the uninterrupted pair transport
// frame-exactly, across loop wraps, for deck-local rolls and reverse-rolls,
// and after an unpair mid-slip.
//
// Coverage: T13 (deck-local roll, both directions), T15a (deck-local reverse
// roll), T25 (marquee slip of the vocal deck), T27 (leader slip + scratch),
// T28 (both members slip), T29 (slip across a loop wrap), T31 (unpair while
// slipping). T30's table/bridge invariants live in deckpairmanager_test.cpp
// and deckpair_test.cpp (Phase 3). The slip-plane == pair-plane assertions
// read EngineBuffer::m_slipPos through the friend declaration in
// enginebuffer.h; the wrap-based tests (T13, T29) run on the real scalers
// because the mock scaler bypasses the reader and never wraps (see the note
// at the top of deckpair_test.cpp).

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "control/controlobject.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/slipmodestate.h"
#include "mixer/deck.h"
#include "mixer/deckpairmanager.h"
#include "test/deckpairtestbase.h"
#include "test/signalpathtest.h"
#include "track/beats.h"
#include "track/track.h"

namespace {
constexpr char kPlay[] = "play";
constexpr char kRate[] = "rate";
constexpr char kSlipEnabled[] = "slip_enabled";

/// Frames a single engine callback advances at normal speed
/// (kProcessBufferSize samples / 2 channels), plus slack for the scaler
/// ramping a buffer.
constexpr double kBufferSlackFrames = 1200.0;
} // namespace

// ---------------------------------------------------------------------------
// Mocked fixture: the transport-lock plane, the slip-plane feed and the deck
// slip state. Frame-exact assertions are meaningful on the mock scaler.
// ---------------------------------------------------------------------------

class DeckPairSlipTest : public DeckPairTestBase {
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

    /// Relative write, the way a controller or skin nudges an accumulator.
    void addRelative(int deckIndex, const QString& item, double delta) {
        setControl(deckIndex, item, control(deckIndex, item) + delta);
    }

    double playPos(int deckIndex) {
        return buffer(deckIndex)->getPlayPos().value();
    }

    /// The buffer's slip plane (snap-back target). Engine thread state, read
    /// after ProcessBuffer() has returned, like the other buffer getters here.
    /// These accessors are the fixture's friend channel into EngineBuffer: a
    /// TEST_F body (derived from the fixture) cannot touch private members
    /// itself.
    double slipPos(int deckIndex) {
        return buffer(deckIndex)->m_slipPos.value();
    }
    /// The slip rate frozen at slip-engage (9.11 d carry-over assertions).
    double slipRate(int deckIndex) {
        return buffer(deckIndex)->m_dSlipRate;
    }
    /// The buffer's sample-consumption-rate bookkeeping (the pair rate while
    /// locked).
    double rateOld(int deckIndex) {
        return buffer(deckIndex)->m_rate_old;
    }

    /// hotcue_*_position / loop_*_position COs are in engine sample units.
    double cueFrames(int deckIndex, const QString& item) const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                control(deckIndex, item))
                .value();
    }
};

// T25 (marquee -- slip the vocal deck): the follower slips and runs its own
// transport (local jog, local hotcue fire), the leader keeps playing
// unaffected, the follower's slip plane *is* the pair plane every callback
// (5.4.3), and the release snaps back frame-exactly. Also pins the 9.11 d
// carry-over: the frozen slip rate taken at engage is the pair's rate, and
// the re-locked follower's rate state is the pair's again after the slip
// release re-anchor.
TEST_F(DeckPairSlipTest, FollowerSlipRunsLocallyAndSnapsBackToThePairPlane) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    // Baseline advance of the locked pair at rate 1.
    double pairBefore = playPos(kLeader);
    ProcessBuffer();
    const double idleAdvance = playPos(kLeader) - pairBefore;
    ASSERT_GT(idleAdvance, 0.0);
    pairBefore = playPos(kLeader);

    // A hotcue stored on the leader (position mirrored to the follower) is
    // the follower's local goto target during its slip.
    pulse(kLeader, QStringLiteral("hotcue_1_setcue"));
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("hotcue_1_status")));
    const double cuePos = cueFrames(kFollower, QStringLiteral("hotcue_1_position"));

    // Slip engages on the follower alone.
    setControl(kFollower, kSlipEnabled, 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    EXPECT_EQ(0.0, control(kLeader, kSlipEnabled));

    // 9.11 d carry-over: slipping takes over as a live deck, so the frozen
    // slip rate must be the pair rate the locked deck replayed until a
    // moment ago -- the follower's m_rate_old follows the snapshot, and the
    // rising edge copies it into m_dSlipRate.
    EXPECT_DOUBLE_EQ(slipRate(kFollower), rateOld(kLeader));

    // Phase A: local jog nudges. The follower is a live deck: it consumes
    // its own `jog` (its accumulator re-baselines like a single deck's),
    // nothing crosses the bridge, and the pair plane is untouched.
    for (int i = 0; i < 3; ++i) {
        addRelative(kFollower, QStringLiteral("jog"), 5.0);
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(0.0, control(kFollower, QStringLiteral("jog")))
                << "a slipping follower consumes its own jog, callback " << i;
        EXPECT_DOUBLE_EQ(0.0, m_pPairManager->pairAccumulated(group(kFollower)))
                << "a slipping follower's gesture must not reach the pair "
                   "lane, callback "
                << i;
        EXPECT_NEAR(playPos(kLeader) - pairBefore, idleAdvance, kBufferSlackFrames)
                << "the leader (pair plane) keeps its own advance, callback " << i;
        pairBefore = playPos(kLeader);
        // 5.4.3: the slip plane tracks the pair plane, frame-exact.
        EXPECT_DOUBLE_EQ(slipPos(kFollower), pairBefore) << "callback " << i;
    }

    // Phase B: a hotcue fired on the slipping follower seeks its *audible*
    // plane only; the slip plane -- the snap-back target -- keeps riding the
    // pair plane.
    pulse(kFollower, QStringLiteral("hotcue_1_goto"));
    ProcessBuffer();
    EXPECT_NEAR(playPos(kLeader) - pairBefore, idleAdvance, kBufferSlackFrames)
            << "the follower's hotcue must not move the pair plane";
    pairBefore = playPos(kLeader);
    EXPECT_NEAR(playPos(kFollower), cuePos, kBufferSlackFrames);
    EXPECT_DOUBLE_EQ(slipPos(kFollower), pairBefore);
    for (int i = 0; i < 2; ++i) {
        ProcessBuffer();
        pairBefore = playPos(kLeader);
        EXPECT_DOUBLE_EQ(slipPos(kFollower), pairBefore) << "callback " << i;
        // audible plane away from the pair plane, and neither member's slip
        // crossed (R8).
        EXPECT_GT(std::fabs(playPos(kFollower) - pairBefore), 2.0 * idleAdvance)
                << "callback " << i;
        EXPECT_EQ(0.0, control(kLeader, kSlipEnabled)) << "callback " << i;
    }

    // The leader's rate moves on while the follower slips: the mirror is
    // disconnected during the slip (9.11 m), the slip-plane feed keeps the
    // snap target on the pair plane regardless.
    setControl(kLeader, kRate, 1.25);
    ProcessBuffer();
    pairBefore = playPos(kLeader);
    EXPECT_DOUBLE_EQ(slipPos(kFollower), pairBefore);

    // Release: the next callback the follower *is* the pair again, and the
    // slip-release re-anchor (9.11 m/9.13) has re-synchronised its rate
    // state with the pair's newest values.
    setControl(kFollower, kSlipEnabled, 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    EXPECT_DOUBLE_EQ(playPos(kFollower), playPos(kLeader))
            << "slip release must snap back onto the pair plane frame-exactly";
    EXPECT_DOUBLE_EQ(1.25, control(kFollower, kRate));
    EXPECT_DOUBLE_EQ(control(kLeader, kRate), control(kFollower, kRate));
    EXPECT_DOUBLE_EQ(control(kLeader, QStringLiteral("rate_ratio")),
            control(kFollower, QStringLiteral("rate_ratio")));
    for (int i = 0; i < 3; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before) << "callback " << i;
    }
}

// T15a (deck-local reverse roll): `reverseroll` on the follower engages slip
// and reverse on its own deck only -- the pair never reverses -- the audible
// plane rolls backwards while the slip plane rides the pair plane, and the
// release snaps forward onto it frame-exactly.
TEST_F(DeckPairSlipTest, ReverseRollOnFollowerIsDeckLocal) {
    startLockedPair();
    // Get well clear of the track start: the reverse roll walks the audible
    // plane backwards and the test measures that walk for several buffers.
    for (int i = 0; i < 8; ++i) {
        ProcessBuffer();
    }

    double pairBefore = playPos(kLeader);
    ProcessBuffer();
    const double idleAdvance = playPos(kLeader) - pairBefore;
    ASSERT_GT(idleAdvance, 0.0);
    pairBefore = playPos(kLeader);

    setControl(kFollower, QStringLiteral("reverseroll"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("reverse")));
    // slotReverseRollActivate sets slip before reverse, and the relay
    // refuses while the follower slips: nothing crossed to the leader.
    EXPECT_EQ(0.0, control(kLeader, kSlipEnabled));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("reverse")))
            << "reverseroll must never touch the pair (R8)";

    double audibleBefore = playPos(kFollower);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        EXPECT_EQ(0.0, control(kLeader, QStringLiteral("reverse")))
                << "callback " << i;
        EXPECT_EQ(0.0, control(kLeader, kSlipEnabled)) << "callback " << i;
        // The pair plane advances uninterrupted ...
        EXPECT_NEAR(playPos(kLeader) - pairBefore, idleAdvance, kBufferSlackFrames)
                << "callback " << i;
        pairBefore = playPos(kLeader);
        // ... and is the follower's snap-back target (5.4.3) ...
        EXPECT_DOUBLE_EQ(slipPos(kFollower), pairBefore) << "callback " << i;
        // ... while its audible plane rolls backwards.
        EXPECT_LT(playPos(kFollower), audibleBefore) << "callback " << i;
        audibleBefore = playPos(kFollower);
    }

    setControl(kFollower, QStringLiteral("reverseroll"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("reverse")));
    EXPECT_DOUBLE_EQ(playPos(kFollower), playPos(kLeader))
            << "reverse-roll release must snap forward onto the pair plane";
    for (int i = 0; i < 3; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before) << "callback " << i;
    }
}

// T27 (leader slips + scratches): the follower stays locked and plays the
// pair plane -- which is the leader's slip plane (5.2) -- uninterrupted,
// frame-exact every callback, while the leader's audible plane wanders in
// scratch mode; the leader's release lands exactly on the follower.
TEST_F(DeckPairSlipTest, LeaderSlipAndScratchKeepsFollowerOnThePairPlane) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("scratch2_indicates_scratching"), 1.0);

    setControl(kLeader, kSlipEnabled, 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kLeader, kSlipEnabled));
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    // Until the scratch starts, audible plane == slip plane == pair plane.
    assertPairLocked();
    EXPECT_DOUBLE_EQ(playPos(kFollower), slipPos(kLeader));

    setControl(kLeader, QStringLiteral("scratch2_enable"), 1.0);
    double followerBefore = playPos(kFollower);
    for (int i = 0; i < 5; ++i) {
        if (i == 2) {
            // Scrub the leader's platter: its audible plane leaves the pair
            // plane, its slip plane (the pair plane) does not care (5.2).
            setControl(kLeader, QStringLiteral("scratch2"), 25.0);
        }
        ProcessBuffer();
        EXPECT_EQ(0.0, control(kFollower, kSlipEnabled)) << "callback " << i;
        // The follower plays the pair plane uninterrupted ...
        EXPECT_GT(playPos(kFollower), followerBefore) << "callback " << i;
        followerBefore = playPos(kFollower);
        // ... and that plane *is* the leader's slip plane, frame-exact.
        EXPECT_DOUBLE_EQ(playPos(kFollower), slipPos(kLeader))
                << "callback " << i;
    }
    // The scratch really moved the leader's audible plane off the pair plane.
    EXPECT_NE(playPos(kLeader), slipPos(kLeader));

    setControl(kLeader, QStringLiteral("scratch2_enable"), 0.0);
    setControl(kLeader, kSlipEnabled, 0.0);
    ProcessBuffer();
    // Release: seekExact(slip plane) on the leader; the follower already
    // *is* that position -- locked again, frame-exact.
    assertPairLocked();
    for (int i = 0; i < 3; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before) << "callback " << i;
    }
}

// T28 (both members slip): each member's gestures are member-local, the
// follower's slip plane tracks the pair plane -- the leader's slip plane
// while the leader rolls -- and interleaved releases converge the members
// back onto one plane.
TEST_F(DeckPairSlipTest, BothMembersSlipIndependentlyAndConvergeOnRelease) {
    startLockedPair();
    setControl(kLeader, QStringLiteral("quantize"), 0.0);
    ProcessBuffer();

    setControl(kFollower, kSlipEnabled, 1.0);
    ProcessBuffer();
    ProcessBuffer();
    setControl(kLeader, kSlipEnabled, 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    ASSERT_EQ(1.0, control(kLeader, kSlipEnabled));
    // Both slip planes ride one pair plane.
    EXPECT_DOUBLE_EQ(slipPos(kFollower), slipPos(kLeader));

    // Leader excursion: its audible plane jumps, the follower's audible
    // plane is not dragged (its slip gesture keeps it local), the pair plane
    // just advances.
    const double followerAudibleBefore = playPos(kFollower);
    setControl(kLeader, QStringLiteral("playposition"), 0.3);
    ProcessBuffer();
    EXPECT_GT(playPos(kLeader) - slipPos(kLeader), 100.0 * kBufferSlackFrames)
            << "the leader excursion must move only the leader's audible plane";
    EXPECT_NEAR(playPos(kFollower) - followerAudibleBefore, 0.0, kBufferSlackFrames)
            << "the leader's excursion must not seek the slipping follower";
    EXPECT_DOUBLE_EQ(slipPos(kFollower), slipPos(kLeader));

    // Follower excursion: same, mirrored.
    const double leaderPairGapBefore = playPos(kLeader) - slipPos(kLeader);
    setControl(kFollower, QStringLiteral("playposition"), 0.6);
    ProcessBuffer();
    EXPECT_GT(playPos(kFollower) - slipPos(kFollower),
            100.0 * kBufferSlackFrames)
            << "the follower excursion must move only the follower's audible "
               "plane";
    EXPECT_NEAR(playPos(kLeader) - slipPos(kLeader), leaderPairGapBefore, kBufferSlackFrames)
            << "the follower's excursion must not seek the leader";
    EXPECT_DOUBLE_EQ(slipPos(kFollower), slipPos(kLeader));

    // The pair plane advances under both slipping members.
    double pairBefore = slipPos(kLeader);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        EXPECT_GT(slipPos(kLeader), pairBefore) << "callback " << i;
        EXPECT_DOUBLE_EQ(slipPos(kFollower), slipPos(kLeader))
                << "callback " << i;
        pairBefore = slipPos(kLeader);
        EXPECT_EQ(1.0, control(kLeader, kSlipEnabled)) << "callback " << i;
        EXPECT_EQ(1.0, control(kFollower, kSlipEnabled)) << "callback " << i;
    }

    // Interleaved release: follower first -- it re-locks onto the pair plane
    // (the leader's slip plane while the leader is still slipping).
    setControl(kFollower, kSlipEnabled, 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    EXPECT_DOUBLE_EQ(playPos(kFollower), slipPos(kLeader));
    EXPECT_EQ(1.0, control(kLeader, kSlipEnabled));
    for (int i = 0; i < 3; ++i) {
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(playPos(kFollower), slipPos(kLeader))
                << "callback " << i;
    }

    // Leader release: the leader snaps onto the same plane; both locked.
    setControl(kLeader, kSlipEnabled, 0.0);
    ProcessBuffer();
    assertPairLocked();
    for (int i = 0; i < 3; ++i) {
        const double before = playPos(kLeader);
        ProcessBuffer();
        assertPairLocked();
        EXPECT_GT(playPos(kLeader), before) << "callback " << i;
    }
}

// T31 (unpair while the follower is slipping): the follower remains a normal
// slipping deck -- slip is not released by disbanding, the slip-plane feed is
// simply gone, so its snap target advances locally at the rate frozen at
// slip-engage; the leader is unaffected; the later release snaps to the
// follower's own plane, away from the ex-partner.
TEST_F(DeckPairSlipTest, UnpairWhileFollowerSlippingLeavesANormalSlippingDeck) {
    startLockedPair();

    double pairBefore = playPos(kLeader);
    ProcessBuffer();
    const double idleAdvance = playPos(kLeader) - pairBefore;
    ASSERT_GT(idleAdvance, 0.0);

    setControl(kFollower, kSlipEnabled, 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    const double frozenRate = slipRate(kFollower);
    EXPECT_DOUBLE_EQ(frozenRate, rateOld(kLeader))
            << "the frozen local rate is the pair's rate at slip-engage";
    for (int i = 0; i < 2; ++i) {
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(slipPos(kFollower), playPos(kLeader))
                << "callback " << i;
    }

    // Unpair from the leader while the follower is mid-slip.
    requestUnpair(kLeader);
    ProcessBuffer();
    EXPECT_EQ(0.0, pairEnabled(kLeader));
    EXPECT_EQ(0.0, pairEnabled(kFollower));
    // The follower stays exactly what it was: a playing, slipping deck.
    EXPECT_EQ(1.0, control(kFollower, kSlipEnabled));
    EXPECT_EQ(1.0, control(kFollower, kPlay));
    EXPECT_DOUBLE_EQ(slipRate(kFollower), frozenRate)
            << "the slip rate stays frozen across the unpair";

    // The leader is unaffected: it simply keeps playing its own transport.
    double leaderBefore = playPos(kLeader);
    ProcessBuffer();
    EXPECT_GT(playPos(kLeader), leaderBefore);

    // The feed is released: the follower's slip plane now advances locally at
    // the frozen rate. Separate the two decks (independent again, no bridge)
    // and watch the frozen plane fall behind the 2x ex-leader.
    setControl(kLeader, QStringLiteral("rate_ratio"), 2.0);
    // Slow the follower's *audible* plane so its release snap-back is
    // observable: jog is consumed by the live deck again.
    double slipBefore = slipPos(kFollower);
    double leaderGap = playPos(kLeader) - slipBefore;
    // The jog low-pass needs a handful of callbacks to reach its plateau;
    // run long enough for a clearly audible lag behind the snap-back plane.
    for (int i = 0; i < 12; ++i) {
        addRelative(kFollower, QStringLiteral("jog"), -8.0);
        ProcessBuffer();
        EXPECT_NEAR(slipPos(kFollower) - slipBefore, idleAdvance, kBufferSlackFrames)
                << "the local slip plane advances at the frozen rate, "
                   "callback "
                << i;
        EXPECT_NEAR((playPos(kLeader) - slipPos(kFollower)) - leaderGap,
                idleAdvance,
                kBufferSlackFrames)
                << "the ex-leader drifts away at the rate delta, callback " << i;
        slipBefore = slipPos(kFollower);
        leaderGap = playPos(kLeader) - slipBefore;
    }
    EXPECT_GT(leaderGap, 3.0 * idleAdvance);
    // The audible plane fell behind the snap-back target during the nudges.
    EXPECT_GT(slipPos(kFollower) - playPos(kFollower), idleAdvance);

    // Release: snaps back onto the follower's own (locally advanced) plane --
    // *not* onto the ex-partner, which is far away by now.
    setControl(kFollower, kSlipEnabled, 0.0);
    const double snapTarget = slipPos(kFollower);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    EXPECT_NEAR(playPos(kFollower), snapTarget, 2.0 * kBufferSlackFrames);
    EXPECT_GT(playPos(kLeader) - playPos(kFollower), 3.0 * idleAdvance)
            << "the unpaired pair members are independent decks now";
    // Both keep playing, nothing stuck.
    leaderBefore = playPos(kLeader);
    const double followerBefore = playPos(kFollower);
    ProcessBuffer();
    EXPECT_GT(playPos(kLeader), leaderBefore);
    EXPECT_GT(playPos(kFollower), followerBefore);
}

// ---------------------------------------------------------------------------
// Real-audio fixture: loop wraps (T13's deck-local roll, T29's snap-back
// inside the pair loop) are executed by the ReadAheadManager while scaling,
// which the mock scaler bypasses entirely.
// ---------------------------------------------------------------------------

class DeckPairSlipAudioTest : public BaseSignalPathTest {
  protected:
    static constexpr int kLeader = 0;   // deck 1
    static constexpr int kFollower = 2; // deck 3

    DeckPairSlipAudioTest() {
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

    ~DeckPairSlipAudioTest() override {
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

    double slipPos(int deckIndex) {
        return buffer(deckIndex)->m_slipPos.value();
    }

    /// Friend-channel accessor (see the note in DeckPairSlipTest).
    SlipModeState slipModeState(int deckIndex) {
        return buffer(deckIndex)->m_slipModeState;
    }

    /// loop_*_position COs are in engine sample units; buffer positions are
    /// frames.
    double loopFrames(int deckIndex, const QString& item) const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                control(deckIndex, item))
                .value();
    }

    /// Rolls until the deck's loop is enabled on both members.
    void enableLoopAndSettle(int deckIndex, const QString& button, int settle = 8) {
        pulse(deckIndex, button);
        for (int i = 0; i < 10 && control(deckIndex, QStringLiteral("loop_enabled")) != 1.0;
                ++i) {
            ProcessBuffer();
        }
        for (int i = 0; i < settle; ++i) {
            ProcessBuffer();
        }
        ASSERT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
        ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    }

    TrackPointer m_pTrackA;
    TrackPointer m_pTrackB;
    std::unique_ptr<DeckPairManager> m_pPairManager;
};

// T13 (★ deck-local roll, follower side; the 4-beat sibling of
// FollowerDeckLocalRollWrapsWhilePairAdvances): the follower's roll wraps on
// its own audible plane while the pair plane advances uninterrupted; the
// leader never slips and never loops; the follower's slip plane *is* the pair
// plane every callback (5.4.3), so the release rejoins it frame-exactly.
TEST_F(DeckPairSlipAudioTest, FollowerRollWrapsLocallyAndSnapsBackFrameExact) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setPlaying(kLeader, true);
    assertPairLocked();

    // SC Live roll-pad dance on the follower: size, then roll activate.
    setControl(kFollower, QStringLiteral("beatloop_size"), 4.0);
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    ASSERT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kLeader, kSlipEnabled));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));

    const double loopStart = loopFrames(kFollower, QStringLiteral("loop_start_position"));
    const double loopEnd = loopFrames(kFollower, QStringLiteral("loop_end_position"));
    ASSERT_GT(loopEnd, loopStart);

    // Held: wrap twice inside the deck-local window; the pair plane keeps
    // advancing and remains the follower's snap-back target every callback.
    ProcessBuffer(); // let the entry seek into the window settle
    int wraps = 0;
    double audibleBefore = playPos(kFollower);
    double leaderBefore = playPos(kLeader);
    for (int i = 0; i < 4000 && wraps < 2; ++i) {
        ProcessBuffer();
        const double audible = playPos(kFollower);
        EXPECT_LE(audible, loopEnd + kBufferSlackFrames)
                << "the roll must stay inside its window, callback " << i;
        if (audible < audibleBefore - kBufferSlackFrames) {
            ++wraps;
            EXPECT_NEAR(audible, loopStart, 2.0 * kBufferSlackFrames)
                    << "wrap " << wraps;
        }
        audibleBefore = audible;
        // The pair plane plays straight through: strictly advancing, and
        // exactly the follower's slip plane (5.4.3).
        EXPECT_GT(playPos(kLeader), leaderBefore)
                << "the pair plane must advance uninterrupted, callback " << i;
        leaderBefore = playPos(kLeader);
        EXPECT_DOUBLE_EQ(slipPos(kFollower), playPos(kLeader))
                << "callback " << i;
        // R8: nothing of the roll crosses to the leader.
        EXPECT_EQ(0.0, control(kLeader, kSlipEnabled)) << "callback " << i;
        EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")))
                << "callback " << i;
    }
    EXPECT_EQ(2, wraps) << "the rolling follower must wrap twice within 4000 "
                           "callbacks";
    EXPECT_GT(playPos(kLeader), loopEnd + kBufferSlackFrames)
            << "the pair plane must have played clear through the roll window";

    // Release: slip releases on the next release write + callback; the snap
    // target is the pair plane, so the rejoin is frame-exact.
    setControl(kFollower, QStringLiteral("beatlooproll_activate"), 0.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// T13 companion (roll held on the pair leader): the follower follows the
// pair plane -- the leader's slip plane while the leader rolls (5.2) -- and
// not the leader's audible roll; the roll's statuses stay off the follower
// (9.16); the leader's release lands back exactly on the follower.
TEST_F(DeckPairSlipAudioTest, LeaderRollRidesThePairPlaneNotTheAudibleRoll) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setPlaying(kLeader, true);
    assertPairLocked();

    setControl(kLeader, QStringLiteral("beatloop_size"), 4.0);
    setControl(kLeader, QStringLiteral("beatlooproll_activate"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kLeader, kSlipEnabled));
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));

    const double rollStart = loopFrames(kLeader, QStringLiteral("loop_start_position"));
    const double rollEnd = loopFrames(kLeader, QStringLiteral("loop_end_position"));
    ASSERT_GT(rollEnd, rollStart);

    ProcessBuffer(); // settle the entry seek
    int wraps = 0;
    double audibleBefore = playPos(kLeader);
    for (int i = 0; i < 4000 && wraps < 2; ++i) {
        ProcessBuffer();
        // The leader's audible plane wraps inside its roll window ...
        const double audible = playPos(kLeader);
        EXPECT_LE(audible, rollEnd + kBufferSlackFrames)
                << "the roll must stay inside its window, callback " << i;
        if (audible < audibleBefore - kBufferSlackFrames) {
            ++wraps;
            EXPECT_NEAR(audible, rollStart, 2.0 * kBufferSlackFrames)
                    << "wrap " << wraps;
        }
        audibleBefore = audible;
        // ... while the follower rides the pair plane (= the leader's slip
        // plane), frame-exact, and sees nothing of the roll (9.16).
        EXPECT_DOUBLE_EQ(playPos(kFollower), slipPos(kLeader))
                << "callback " << i;
        EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")))
                << "callback " << i;
        EXPECT_EQ(0.0, control(kFollower, kSlipEnabled)) << "callback " << i;
    }
    EXPECT_EQ(2, wraps) << "the rolling leader must wrap twice";
    // The pair plane (and with it the follower) played clear through the
    // roll window while the leader audibly looped.
    EXPECT_GT(playPos(kFollower), rollEnd + kBufferSlackFrames);

    // Release: the leader snaps back onto the pair plane -- exactly the
    // follower's position; the vanilla release leaves no loop on either deck
    // (there was no pre-existing pair loop to re-establish, 9.14).
    setControl(kLeader, QStringLiteral("beatlooproll_activate"), 0.0);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kLeader, kSlipEnabled));
    EXPECT_EQ(0.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(0.0, control(kFollower, QStringLiteral("loop_enabled")));
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// T29 (slip + active pair loop across the loop-end boundary): the follower's
// slip plane is fed from the pair plane, which is loop-aware by construction
// (the leader's loop machinery produced it), so it stays inside the pair's
// loop region across wraps and the snap-back lands inside it. SlipModeState
// keeps coming from the follower's own loop replica (5.4.3).
TEST_F(DeckPairSlipAudioTest, FollowerSlipAcrossLoopWrapSnapsBackInsideTheLoop) {
    expectPaired();
    ASSERT_TRUE(waitForEngineTrack(kFollower, m_pTrackA));
    setPlaying(kLeader, true);
    assertPairLocked();

    enableLoopAndSettle(kLeader, QStringLiteral("beatloop_4_activate"));
    const double loopStart = loopFrames(kLeader, QStringLiteral("loop_start_position"));
    const double loopEnd = loopFrames(kLeader, QStringLiteral("loop_end_position"));
    ASSERT_GT(loopEnd, loopStart);

    setControl(kFollower, kSlipEnabled, 1.0);
    ProcessBuffer();
    ASSERT_EQ(1.0, control(kFollower, kSlipEnabled));
    EXPECT_EQ(0.0, control(kLeader, kSlipEnabled));

    // Run at least one full pass of the pair's loop: the slip plane (snap
    // target) stays locked to the pair plane and inside the pair's region,
    // across the wrap.
    int wraps = 0;
    double slipBefore = slipPos(kFollower);
    for (int i = 0; i < 800 && wraps < 1; ++i) {
        ProcessBuffer();
        const double slip = slipPos(kFollower);
        EXPECT_DOUBLE_EQ(slip, playPos(kLeader)) << "callback " << i;
        EXPECT_GE(slip, loopStart - kBufferSlackFrames) << "callback " << i;
        EXPECT_LE(slip, loopEnd + kBufferSlackFrames) << "callback " << i;
        // Slip mode state comes from the follower's own loop replica (pair
        // loop mirrored before slip-engage): the snap-back is armed.
        EXPECT_EQ(SlipModeState::Armed, slipModeState(kFollower))
                << "callback " << i;
        if (slip < slipBefore - kBufferSlackFrames) {
            ++wraps;
        }
        slipBefore = slip;
    }
    EXPECT_GE(wraps, 1) << "the pair plane must complete at least one loop pass";

    // Release: the snap-back lands on the pair plane inside the pair's loop
    // region, frame-exact, and the lock runs on.
    setControl(kFollower, kSlipEnabled, 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(kFollower, kSlipEnabled));
    assertPairLocked();
    EXPECT_GE(playPos(kFollower), loopStart - kBufferSlackFrames);
    EXPECT_LE(playPos(kFollower), loopEnd + kBufferSlackFrames);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
    }
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
