// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// Tests for the deck pair surface and lifecycle: the pair_enabled control,
// validation, roles, teardown, track adoption, load/eject coordination and
// the neutralisation of follower-side transport authorities.
//
// DECK_PAIR_PLAN.md tests T2 - T8 (Phase 1). T1 lives in
// deckpairtransport_test.cpp.

#include "mixer/deckpairmanager.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <atomic>
#include <chrono>
#include <thread>

#include "control/controlobject.h"
#include "engine/deckpairtransport.h"
#include "engine/enginebuffer.h"
#include "mixer/deck.h"
#include "test/deckpairtestbase.h"

// Backport note (2.6): on this branch LoadWhenDeckPlaying and the
// "[Controls]" keys live in preferences/dialog/dlgprefdeck.h, which drags in
// the generated dialog UI header and so cannot be included from the test
// target (2.7 moved the enum to preferences/interface.h and stopped exporting
// the keys). Mirror the definitions here instead, exactly as the 2.7 test does
// for the key; keep in sync with dlgprefdeck.h.
enum class LoadWhenDeckPlaying {
    Reject,
    Allow,
    AllowButStopDeck
};
namespace {
const ConfigKey kConfigKeyLoadWhenDeckPlaying =
        ConfigKey(QStringLiteral("[Controls]"), QStringLiteral("LoadWhenDeckPlaying"));
} // namespace

class DeckPairManagerTest : public DeckPairTestBase {
  protected:
    void loadOnDeck(int deckIndex, const TrackPointer& pTrack) {
        deck(deckIndex)->slotLoadTrack(pTrack,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
    }

    /// Makes the deck look like it has a configured passthrough/vinyl input,
    /// which is a precondition of both controls being confirmed at all.
    void forceInputConfigured(int deckIndex) {
        ControlObject* pConfigured = ControlObject::getControl(
                ConfigKey(group(deckIndex), QStringLiteral("input_configured")));
        ASSERT_NE(nullptr, pConfigured);
        pConfigured->forceSet(1.0);
    }

    /// Since the Phase 2 transport lock the members of a pair share their
    /// play state: the follower's commands are relayed to the leader and the
    /// leader's play state is mirrored back. The one supported way for a
    /// single member to play while the partner is stopped is slip -- a
    /// slipping follower drives its own transport and its commands stay
    /// local (R8). The "load when deck playing" tests use this to build a
    /// playing partner.
    void makeFollowerPlayLocally(int leaderIndex, int followerIndex) {
        // Start from a clearly stopped pair.
        setControl(leaderIndex, QStringLiteral("play"), 0.0);
        ProcessBuffer();
        ASSERT_EQ(0.0, control(followerIndex, QStringLiteral("play")));
        // Slip first: from now on the bridge is disconnected for this member.
        setControl(followerIndex, QStringLiteral("slip_enabled"), 1.0);
        ProcessBuffer();
        setControl(followerIndex, QStringLiteral("play"), 1.0);
        ProcessBuffer();
        ASSERT_EQ(1.0, control(followerIndex, QStringLiteral("play")));
        ASSERT_EQ(0.0, control(leaderIndex, QStringLiteral("play")));
    }

    /// Re-forms a pair that a refused propagation has disbanded, and leaves it
    /// as the previous case left it: follower playing locally.
    ///
    /// A refused propagation disbands (DeckPairManager::propagateTrack,
    /// DECK_PAIR_PLAN.md 5.7): a locked follower replays the leader's frame
    /// positions against whatever track it holds itself, so a pair whose
    /// members hold different tracks is not a degraded pair -- it is two
    /// unrelated tracks at one transport, with nothing to detect or heal it.
    void reFormPairWithPlayingFollower(int leaderIndex, int followerIndex) {
        setControl(followerIndex, QStringLiteral("slip_enabled"), 0.0);
        ProcessBuffer();
        setControl(followerIndex, QStringLiteral("play"), 0.0);
        setControl(leaderIndex, QStringLiteral("play"), 0.0);
        ProcessBuffer();
        ASSERT_FALSE(m_pPairManager->isPaired(group(leaderIndex)));
        // Let a loaded leader settle before re-pairing: formation adopts the
        // leader's track, and a settled leader is what a DJ actually presents
        // (unpair, then pair again a moment later). An empty leader instead
        // adopts the *follower's* track (5.7), so there is nothing to settle.
        const TrackPointer pLeaderTrack = deck(leaderIndex)->getLoadedTrack();
        if (pLeaderTrack) {
            ASSERT_TRUE(waitForEngineTrack(leaderIndex, pLeaderTrack));
        }
        expectPaired(leaderIndex);
        // Formation adopts a track on whichever member was empty, so both end
        // up on the same one.
        const TrackPointer pPairTrack = deck(leaderIndex)->getLoadedTrack();
        ASSERT_TRUE(pPairTrack) << "the pair holds no track after formation";
        ASSERT_TRUE(waitForEngineTrack(followerIndex, pPairTrack));
        ASSERT_EQ(pPairTrack, deck(followerIndex)->getLoadedTrack())
                << "the pair members must hold the same track after formation";
        makeFollowerPlayLocally(leaderIndex, followerIndex);
    }
};

// T2: pairing is initiated from either deck; the deck the write lands on is
// the leader and its track becomes the pair's track.
TEST_F(DeckPairManagerTest, PairFromThirdDeckMakesItTheLeader) {
    registerDecks(4);
    ASSERT_EQ(4, m_pPairManager->numberOfDecks());
    ASSERT_EQ(0.0, pairEnabled(2));
    // Deck 1 (the future follower) holds a different track than deck 3.
    ASSERT_EQ(m_pTrackA, deck(0)->getLoadedTrack());
    ASSERT_EQ(m_pTrackB, deck(2)->getLoadedTrack());

    // Deck 3 (index 2) initiates, its partner deck 1 (index 0) is idle.
    expectPaired(2);

    EXPECT_EQ(2, DeckPairManager::partnerDeckIndex(0));
    EXPECT_EQ(3, DeckPairManager::partnerDeckIndex(1));
    EXPECT_TRUE(m_pPairManager->isPaired(group(0)));
    EXPECT_EQ(group(0), m_pPairManager->partnerGroup(group(2)));
    EXPECT_EQ(group(2), m_pPairManager->partnerGroup(group(0)));

    // The engine buffers report the roles, and both point at the same
    // transport instance.
    EXPECT_EQ(PairRole::PairLeader, buffer(2)->pairRole());
    EXPECT_EQ(PairRole::PairFollower, buffer(0)->pairRole());
    EXPECT_TRUE(buffer(0)->isPaired());
    ASSERT_NE(nullptr, buffer(0)->pairTransport());
    EXPECT_EQ(buffer(0)->pairTransport(), buffer(2)->pairTransport());
    EXPECT_TRUE(buffer(0)->pairTransport()->isActive());

    // The follower adopted the leader's track, the leader is untouched (R3/R4).
    EXPECT_EQ(m_pTrackB, deck(0)->getLoadedTrack());
    EXPECT_EQ(m_pTrackB, deck(2)->getLoadedTrack());
#ifdef __STEM__
    // Stem selection is per deck (R9): adopting the track keeps it.
    EXPECT_EQ(m_pTrackB->hasStem(), deck(0)->getLoadedTrack()->hasStem());
#endif
    EXPECT_TRUE(waitForEngineTrack(0, m_pTrackB));
}

// T3: pairing is refused while the partner is playing.
TEST_F(DeckPairManagerTest, RefusesPairingWhilePartnerIsPlaying) {
    registerDecks(4);
    ASSERT_NE(2, DeckPairManager::partnerDeckIndex(1));
    setPlaying(0, true);
    ASSERT_EQ(1.0, control(0, QStringLiteral("play")));
    const TrackPointer leaderTrackBefore = deck(2)->getLoadedTrack();

    requestPair(2);

    EXPECT_EQ(0.0, pairEnabled(2));
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_FALSE(m_pPairManager->isPaired(group(2)));
    EXPECT_EQ(PairRole::Unpaired, buffer(2)->pairRole());
    EXPECT_EQ(PairRole::Unpaired, buffer(0)->pairRole());
    // Nothing else changed: both decks keep their own track and the partner
    // keeps playing.
    EXPECT_EQ(1.0, control(0, QStringLiteral("play")));
    EXPECT_EQ(leaderTrackBefore, deck(2)->getLoadedTrack());
    EXPECT_NE(m_pTrackB, deck(0)->getLoadedTrack());
}

// T4: the partner deck must exist. Decks only ever grow, so a formed pair
// never loses its partner.
TEST_F(DeckPairManagerTest, RefusesPairingWithoutPartnerDeck) {
    registerDecks(2);
    ASSERT_EQ(2, m_pPairManager->numberOfDecks());
    ASSERT_EQ(m_pTrackA, deck(0)->getLoadedTrack());

    requestPair(0);
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));

    // Growing the deck count to 4 makes decks 2+4 pairable.
    registerDecks(4);
    EXPECT_EQ(4, m_pPairManager->numberOfDecks());
    expectPaired(1);
    EXPECT_TRUE(m_pPairManager->isPaired(group(3)));
}

// T5: unpairing from either member ends the pair, playback continues.
TEST_F(DeckPairManagerTest, UnpairFromFollowerEndsPairAndKeepsPlaying) {
    registerDecks(4);
    expectPaired(0); // leader deck 1, follower deck 3
    // The follower adopted the leader's track asynchronously.
    ASSERT_TRUE(waitForEngineTrack(2, deck(0)->getLoadedTrack()));
    setPlaying(0, true);
    setPlaying(2, true);
    ASSERT_EQ(1.0, control(0, QStringLiteral("play")));
    ASSERT_EQ(1.0, control(2, QStringLiteral("play")));

    requestUnpair(2);

    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_EQ(0.0, pairEnabled(2));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));
    EXPECT_TRUE(m_pPairManager->partnerGroup(group(0)).isEmpty());
    EXPECT_EQ(PairRole::Unpaired, buffer(0)->pairRole());
    EXPECT_EQ(PairRole::Unpaired, buffer(2)->pairRole());
    EXPECT_EQ(nullptr, buffer(0)->pairTransport());
    EXPECT_EQ(nullptr, buffer(2)->pairTransport());

    // Uninterrupted playback (R6).
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_EQ(1.0, control(0, QStringLiteral("play")));
    EXPECT_EQ(1.0, control(2, QStringLiteral("play")));
    EXPECT_TRUE(buffer(0)->getPlayPos().isValid());
    EXPECT_TRUE(buffer(2)->getPlayPos().isValid());

    // Without a pair, a seek on the former follower is its own business and
    // the ex-leader is not dragged along (there is no relay in Phase 1).
    const double leaderPosBefore = buffer(2)->getPlayPos().value();
    const double followerPosBefore = buffer(0)->getPlayPos().value();
    buffer(0)->seekExact(mixxx::audio::FramePos::fromEngineSamplePos(500000));
    ProcessBuffer();
    EXPECT_GT(buffer(0)->getPlayPos().value(), followerPosBefore);
    EXPECT_GT(buffer(2)->getPlayPos().value(), leaderPosBefore);
    EXPECT_NE(buffer(0)->getPlayPos().value(), buffer(2)->getPlayPos().value());
}

// T5b: a pair cannot be formed twice, and unpairing is a no-op when unpaired.
TEST_F(DeckPairManagerTest, SecondPairingRequestIsRefused) {
    registerDecks(4);
    expectPaired(0);

    // The partner must not start a second pair.
    requestPair(2);
    EXPECT_EQ(1.0, pairEnabled(2));
    EXPECT_EQ(1.0, pairEnabled(0));
    EXPECT_EQ(PairRole::PairLeader, m_pPairManager->pairRole(group(0)));

    // Nor may the pair be hijacked by an unrelated slot request.
    expectPaired(1); // decks 2+4, independent slot
    EXPECT_TRUE(m_pPairManager->isPaired(group(3)));

    requestUnpair(1);
    requestUnpair(1); // no-op
    EXPECT_EQ(0.0, pairEnabled(1));
    EXPECT_EQ(0.0, pairEnabled(3));
}

// T6: a load on either member loads the same track on both.
TEST_F(DeckPairManagerTest, LoadOnEitherMemberLoadsBoth) {
    registerDecks(4);
    expectPaired(2); // leader deck 3, follower deck 1
    ASSERT_EQ(m_pTrackB, deck(0)->getLoadedTrack());

    // Load on the leader.
    int followerLoads = 0;
    QObject::connect(deck(0),
            &BaseTrackPlayer::loadingTrack,
            [&followerLoads](TrackPointer, TrackPointer) { ++followerLoads; });
    loadOnDeck(2, m_pTrackC);
    EXPECT_EQ(m_pTrackC, deck(2)->getLoadedTrack());
    EXPECT_EQ(m_pTrackC, deck(0)->getLoadedTrack());
    EXPECT_EQ(1, followerLoads) << "the load must be propagated exactly once";
    EXPECT_TRUE(waitForEngineTrack(0, m_pTrackC));

    // Load on the follower: applied to the leader as well, because the pair
    // acts as a single deck.
    int leaderLoads = 0;
    QObject::connect(deck(2),
            &BaseTrackPlayer::loadingTrack,
            [&leaderLoads](TrackPointer, TrackPointer) { ++leaderLoads; });
    loadOnDeck(0, m_pTrackA);
    EXPECT_EQ(m_pTrackA, deck(2)->getLoadedTrack());
    EXPECT_EQ(m_pTrackA, deck(0)->getLoadedTrack());
    EXPECT_EQ(1, leaderLoads);
    EXPECT_TRUE(waitForEngineTrack(2, m_pTrackA));

    // Unpaired decks are not affected.
    requestUnpair(2);
    loadOnDeck(0, m_pTrackB);
    EXPECT_EQ(m_pTrackB, deck(0)->getLoadedTrack());
    EXPECT_EQ(m_pTrackA, deck(2)->getLoadedTrack());
}

// T6b: propagating a load to the partner honours the "load when deck playing"
// safeguard exactly like a library load into that deck: a pair must not be a
// way to override it.
TEST_F(DeckPairManagerTest, PropagatedLoadHonoursLoadWhenDeckPlaying) {
    registerDecks(4);
    expectPaired(0); // leader deck 1, follower deck 3
    ASSERT_TRUE(waitForEngineTrack(2, deck(0)->getLoadedTrack()));

    // The follower plays (locally, by slipping -- see the helper) while the
    // leader is stopped.
    makeFollowerPlayLocally(0, 2);
    ASSERT_EQ(1.0, control(2, QStringLiteral("play")));

    // Reject: the partner keeps both its track and its playback, and the pair
    // is disbanded rather than left locked across two different tracks. The
    // DJ's own load wins over the pair; both decks stay coherent.
    config()->setValue(kConfigKeyLoadWhenDeckPlaying,
            static_cast<int>(LoadWhenDeckPlaying::Reject));
    loadOnDeck(0, m_pTrackC);
    EXPECT_EQ(m_pTrackC, deck(0)->getLoadedTrack());
    EXPECT_NE(m_pTrackC, deck(2)->getLoadedTrack());
    EXPECT_EQ(1.0, control(2, QStringLiteral("play")));
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_EQ(0.0, pairEnabled(2));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));

    // Allow: the partner loads and goes on playing the new track.
    reFormPairWithPlayingFollower(0, 2);
    config()->setValue(kConfigKeyLoadWhenDeckPlaying,
            static_cast<int>(LoadWhenDeckPlaying::Allow));
    loadOnDeck(0, m_pTrackB);
    EXPECT_EQ(m_pTrackB, deck(2)->getLoadedTrack());
    EXPECT_TRUE(waitForEngineTrack(2, m_pTrackB));
    EXPECT_EQ(1.0, control(2, QStringLiteral("play")));

    // Allow, but stop deck: the partner loads and stops.
    config()->setValue(kConfigKeyLoadWhenDeckPlaying,
            static_cast<int>(LoadWhenDeckPlaying::AllowButStopDeck));
    loadOnDeck(0, m_pTrackA);
    EXPECT_EQ(m_pTrackA, deck(2)->getLoadedTrack());
    ProcessBuffer();
    EXPECT_EQ(0.0, control(2, QStringLiteral("play")));

    // The legacy flag alone still means "allow".
    config()->remove(kConfigKeyLoadWhenDeckPlaying);
    config()->setValue(ConfigKey(QStringLiteral("[Controls]"),
                               QStringLiteral("AllowTrackLoadToPlayingDeck")),
            1);
    loadOnDeck(0, m_pTrackC);
    EXPECT_EQ(m_pTrackC, deck(2)->getLoadedTrack());
}

// T7b: ejecting one member is gated by the same "load when deck playing"
// safeguard as a load: the pair must not be a way to empty a playing deck.
TEST_F(DeckPairManagerTest, EjectHonoursLoadWhenDeckPlaying) {
    registerDecks(4);
    expectPaired(0);
    ASSERT_TRUE(waitForEngineTrack(2, deck(0)->getLoadedTrack()));
    makeFollowerPlayLocally(0, 2);
    ASSERT_EQ(1.0, control(2, QStringLiteral("play")));

    // Reject: the stopped member is ejected, the playing partner is left
    // alone, and the pair is disbanded instead of being kept locked across a
    // loaded and an empty deck.
    config()->setValue(kConfigKeyLoadWhenDeckPlaying,
            static_cast<int>(LoadWhenDeckPlaying::Reject));
    deck(0)->slotEjectTrack(1.0);
    EXPECT_EQ(nullptr, deck(0)->getLoadedTrack());
    ASSERT_TRUE(deck(2)->getLoadedTrack());
    EXPECT_EQ(1.0, control(2, QStringLiteral("play")));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));

    // Allow: ejecting the pair clears the playing partner as well, stopping it.
    reFormPairWithPlayingFollower(0, 2);
    config()->setValue(kConfigKeyLoadWhenDeckPlaying,
            static_cast<int>(LoadWhenDeckPlaying::Allow));
    // The partner is still playing (and the leader is empty). A pair load
    // restores the leader's track and keeps the partner playing.
    loadOnDeck(0, m_pTrackB);
    ASSERT_EQ(m_pTrackB, deck(2)->getLoadedTrack());
    // Playback resumes once the asynchronous load has finished. The load also
    // ends the follower's local slip, so the pair is fully bridged again.
    ASSERT_TRUE(waitForEngineTrack(2, m_pTrackB));
    ProcessBuffer();
    ASSERT_EQ(1.0, control(2, QStringLiteral("play")));

    deck(0)->slotEjectTrack(1.0);
    EXPECT_EQ(nullptr, deck(0)->getLoadedTrack());
    EXPECT_EQ(nullptr, deck(2)->getLoadedTrack());
    EXPECT_FALSE(buffer(2)->isTrackLoaded());
    EXPECT_EQ(0.0, control(2, QStringLiteral("play")));
    EXPECT_EQ(1.0, pairEnabled(0));
}

// T7: ejecting one member ejects both, the pair stays engaged and can be
// loaded again.
TEST_F(DeckPairManagerTest, EjectOnEitherMemberEmptiesBoth) {
    registerDecks(4);
    expectPaired(0);
    ASSERT_TRUE(deck(0)->getLoadedTrack());
    ASSERT_TRUE(deck(2)->getLoadedTrack());
    // Let the adoption load finish before ejecting again.
    ASSERT_TRUE(waitForEngineTrack(2, deck(0)->getLoadedTrack()));

    deck(0)->slotEjectTrack(1.0);
    EXPECT_EQ(nullptr, deck(0)->getLoadedTrack());
    EXPECT_EQ(nullptr, deck(2)->getLoadedTrack());
    // Ejecting is synchronous on the engine side.
    EXPECT_FALSE(buffer(2)->isTrackLoaded());

    // An empty pair behaves like one empty deck and stays engaged.
    EXPECT_EQ(1.0, pairEnabled(0));
    EXPECT_EQ(1.0, pairEnabled(2));
    EXPECT_TRUE(m_pPairManager->isPaired(group(0)));

    // The first load lands on both.
    loadOnDeck(2, m_pTrackC);
    EXPECT_EQ(m_pTrackC, deck(0)->getLoadedTrack());
    EXPECT_EQ(m_pTrackC, deck(2)->getLoadedTrack());
    EXPECT_TRUE(waitForEngineTrack(0, m_pTrackC));
    EXPECT_TRUE(waitForEngineTrack(2, m_pTrackC));
}

// T8: sync and vinyl control are refused on a pair follower, passthrough ends
// the pair.
TEST_F(DeckPairManagerTest, FollowerRefusesItsOwnTransportAuthorities) {
    registerDecks(4);
    expectPaired(0); // leader deck 1, follower deck 3
    forceInputConfigured(2);

    setControl(2, QStringLiteral("sync_enabled"), 1.0);
    EXPECT_EQ(0.0, control(2, QStringLiteral("sync_enabled")));
    ProcessBuffer();
    EXPECT_EQ(0.0, control(2, QStringLiteral("sync_enabled")));
    EXPECT_EQ(0.0, control(2, QStringLiteral("sync_mode")));

    setControl(2, QStringLiteral("sync_mode"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(2, QStringLiteral("sync_mode")));

    setControl(2, QStringLiteral("vinylcontrol_enabled"), 1.0);
    EXPECT_EQ(0.0, control(2, QStringLiteral("vinylcontrol_enabled")));
    setControl(2, QStringLiteral("vinylcontrol_wantenabled"), 1.0);
    EXPECT_EQ(0.0, control(2, QStringLiteral("vinylcontrol_wantenabled")));

    // The leader is free to sync with other decks.
    setControl(0, QStringLiteral("sync_enabled"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, control(0, QStringLiteral("sync_enabled")));
    setControl(0, QStringLiteral("sync_enabled"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(0.0, control(0, QStringLiteral("sync_enabled")));

    // An unpaired deck keeps its sync/vinyl controls.
    requestUnpair(2);
    setControl(2, QStringLiteral("sync_enabled"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, control(2, QStringLiteral("sync_enabled")));
    setControl(2, QStringLiteral("sync_enabled"), 0.0);
    ProcessBuffer();
}

// T8b: passthrough and deck pairs are mutually exclusive; enabling
// passthrough on a member disbands the pair.
TEST_F(DeckPairManagerTest, PassthroughDisbandsThePair) {
    registerDecks(4);
    expectPaired(0);
    forceInputConfigured(2);

    setControl(2, QStringLiteral("passthrough"), 1.0);

    // The user request wins: passthrough stays on, the pair is gone.
    EXPECT_EQ(1.0, control(2, QStringLiteral("passthrough")));
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_EQ(0.0, pairEnabled(2));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));
    EXPECT_EQ(PairRole::Unpaired, buffer(0)->pairRole());
    EXPECT_EQ(PairRole::Unpaired, buffer(2)->pairRole());

    setControl(2, QStringLiteral("passthrough"), 0.0);

    // While passthrough is active a new pair is refused.
    setControl(2, QStringLiteral("passthrough"), 1.0);
    requestPair(0);
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));
    setControl(2, QStringLiteral("passthrough"), 0.0);

    // The leader is the dangerous member: with passthrough on the deck that
    // owns the shared transport, the follower would keep replaying a transport
    // whose audio is muted to the crossfader by the input. Disband, don't
    // degrade.
    requestPair(0);
    ASSERT_TRUE(m_pPairManager->isPaired(group(0)));
    forceInputConfigured(0);
    setControl(0, QStringLiteral("passthrough"), 1.0);
    EXPECT_EQ(1.0, control(0, QStringLiteral("passthrough")));
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));
    EXPECT_EQ(PairRole::Unpaired, buffer(0)->pairRole());
    EXPECT_EQ(PairRole::Unpaired, buffer(2)->pairRole());
}

// Hardening (Phase 5): passthrough and pair_enabled may be written from a
// controller thread -- that is what vinyl control and MIDI mappings do. Both
// handlers form or disband a pair, which creates and destroys the members'
// ControlProxies and rewires the bridge; none of that may run on the writing
// thread. The listeners therefore use AutoConnection (DECK_PAIR_PLAN.md 9.18),
// which keeps a same-thread write synchronous and defers a foreign one.
TEST_F(DeckPairManagerTest, PassthroughFromAnotherThreadDisbandsSafely) {
    registerDecks(4);
    expectPaired(0);
    forceInputConfigured(2);

    QThread* pMainThread = QThread::currentThread();
    std::atomic_bool written{false};

    // A real controller thread writes the control: ControlObject::set() from a
    // foreign thread is exactly the situation the connection type decides.
    std::thread writer([this, pMainThread, &written] {
        ControlObject* pPassthrough = ControlObject::getControl(
                ConfigKey(group(2), QStringLiteral("passthrough")));
        if (pPassthrough != nullptr) {
            pPassthrough->set(1.0);
        }
        written.store(QThread::currentThread() != pMainThread &&
                pPassthrough != nullptr);
    });
    writer.join();
    ASSERT_TRUE(written.load()) << "the writer thread never ran the write";

    // The write happened on another thread, so the pair is still intact -- it
    // has not been taken apart on a controller thread.
    EXPECT_TRUE(m_pPairManager->isPaired(group(0)))
            << "a controller-thread write tore the pair down on its own thread";

    // Deliver it: the disband runs on the manager's thread.
    QCoreApplication::processEvents();
    EXPECT_FALSE(m_pPairManager->isPaired(group(0)));
    EXPECT_EQ(0.0, pairEnabled(0));
    EXPECT_EQ(PairRole::Unpaired, buffer(0)->pairRole());
    EXPECT_EQ(1.0, control(2, QStringLiteral("passthrough")));
}

// Formation adoption must not depend on the leader's *engine* load having
// finished. cloneFromChannel() takes the track from the source's engine buffer
// and returns silently while a load is in flight there, which used to leave a
// freshly formed pair locked across two different tracks -- the state Phase 5
// calls the one thing a pair must never be. SynchronizeTracks() now falls back
// to a plain load of the leader's deck-level track; this is the case that
// reaches that branch (no settle wait anywhere), and the point of it is the
// same-track assertion, not the timing.
TEST_F(DeckPairManagerTest, FormingWhileTheLeaderIsStillLoadingStillAdopts) {
    registerDecks(4);
    // Put the leader on a track it has not finished loading yet: the deck-level
    // pointer is set straight away, the engine buffer follows later.
    loadOnDeck(0, m_pTrackC);
    ASSERT_EQ(m_pTrackC, deck(0)->getLoadedTrack());

    // Pair immediately: no ProcessBuffer, no waiting for the engine.
    setControl(0, QStringLiteral("pair_enabled"), 1.0);
    EXPECT_TRUE(m_pPairManager->isPaired(group(0))) << "PROBE not paired";
    EXPECT_TRUE(m_pTrackC == deck(0)->getLoadedTrack()) << "PROBE leader deck track";
    // Backport note (2.6): on this branch EngineBuffer::getLoadedTrack() keeps
    // reporting the *previously* loaded track until the async load completes
    // (2.7 reports the requested track synchronously, which is what the
    // original assertion pinned). The engine may still hold the old track at
    // this point; that is the mid-load state this test exists to construct.
    // The adoption itself is pinned by the follower assertions below, and the
    // engine-side convergence by waitForEngineTrack() at the end.
    EXPECT_TRUE(buffer(0)->isTrackLoaded()) << "PROBE leader engine not Ready";
    EXPECT_TRUE(m_pTrackC == deck(2)->getLoadedTrack())
            << "PROBE follower deck track right after pairing";
    ProcessBuffer();
    EXPECT_TRUE(m_pTrackC == deck(2)->getLoadedTrack())
            << "PROBE follower deck track after one callback";

    // And it is not just a deck-level coincidence: the engine converges on the
    // same track for both members.
    EXPECT_TRUE(waitForEngineTrack(0, m_pTrackC));
    EXPECT_TRUE(waitForEngineTrack(2, m_pTrackC));
}

// T35: pairing an empty leader adopts the follower's track instead of
// discarding it; a load afterwards propagates.
TEST_F(DeckPairManagerTest, PairFormationWithEmptyLeaderKeepsTrack) {
    registerDecks(4);
    deck(0)->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck(0)->getLoadedTrack());
    const TrackPointer followerTrack = deck(2)->getLoadedTrack();
    ASSERT_TRUE(followerTrack);

    expectPaired(0);

    // The pair is one deck with the leader's (empty) transport authority, but
    // no loaded track is silently thrown away.
    EXPECT_EQ(followerTrack, deck(0)->getLoadedTrack());
    EXPECT_EQ(followerTrack, deck(2)->getLoadedTrack());
    EXPECT_TRUE(m_pPairManager->isPaired(group(0)));
}

// T30: the no-slip-keys invariant (DECK_PAIR_PLAN.md 5.5 / R8). The bridge is
// a pure two-table lookup and neither table may contain any slip-related key
// in either direction: slip and its snap-back are strictly deck-specific.
// This static check is the guard that keeps the engine-level slip behaviour
// honest: if anyone ever relays or mirrors a slip key, this fails before the
// audio does.
TEST_F(DeckPairManagerTest, BridgeTablesNeverContainSlipKeys) {
    const QStringList relay = DeckPairManager::bridgeRelayKeys();
    const QStringList mirror = DeckPairManager::bridgeMirrorKeys();

    // Sanity: the tables are populated.
    EXPECT_FALSE(relay.isEmpty());
    EXPECT_FALSE(mirror.isEmpty());

    // A key is slip-related if it is slip state or a slip gesture: slip state
    // itself, any roll / reverse-roll variant (beatlooproll_<size>_activate,
    // beatlooproll_r<size>_activate, beatloop_r<size>_toggle, reverseroll) --
    // everything whose semantics are "own transport until released".
    const auto isSlipKey = [](const QString& key) {
        return key == QLatin1String("slip_enabled") ||
                key == QLatin1String("reverseroll") ||
                key.startsWith(QLatin1String("slip_")) ||
                key.startsWith(QLatin1String("beatlooproll")) ||
                key.startsWith(QLatin1String("beatloop_r"));
    };

    for (const QString& key : relay) {
        EXPECT_FALSE(isSlipKey(key))
                << "relay table (F->L) must never carry a slip key: "
                << key.toUtf8().constData();
    }
    for (const QString& key : mirror) {
        EXPECT_FALSE(isSlipKey(key))
                << "mirror table (L->F) must never carry a slip key: "
                << key.toUtf8().constData();
    }

    // `jog` is the consumer-zeroed accumulator: it crosses on the pair
    // transport's accumulator lane, never on the CO bridge -- a copy relay
    // feeds the leader's already-applied deltas back (bench 9.12 i) and a
    // drain-move sums absolute-velocity writes (bench 9.13); both pass every
    // write-once-per-callback test and fail on hardware. Pin its absence from
    // both tables so the lane stays the only path.
    EXPECT_FALSE(relay.contains(QLatin1String("jog")))
            << "`jog` must not be bridged; it rides the accumulator lane "
               "(DECK_PAIR_PLAN.md 9.13)";
    EXPECT_FALSE(mirror.contains(QLatin1String("jog")))
            << "`jog` is not leader state; it rides the accumulator lane "
               "(DECK_PAIR_PLAN.md 9.13)";
}

// T30 companion: the other "never bridged" families of DECK_PAIR_PLAN.md
// 5.5 -- the mixer strip and stem properties (R9), the transport-authority
// and lifecycle controls that are refused or coordinated elsewhere (5.1,
// 5.7, 5.8). The bridge may only ever talk about the shared *deck*.
TEST_F(DeckPairManagerTest, BridgeTablesExcludeEverythingThatIsNotTheDeck) {
    const QStringList tables = DeckPairManager::bridgeRelayKeys() +
            DeckPairManager::bridgeMirrorKeys();
    ASSERT_FALSE(tables.isEmpty());

    const auto forbidden = [](const QString& key) -> bool {
        // Mixer strip (R9): never shared within a pair.
        static const QStringList stripPrefixes = {
                QStringLiteral("volume"),
                QStringLiteral("pregain"),
                QStringLiteral("orientation"),
                QStringLiteral("kill"),
                QStringLiteral("filter"),
                QStringLiteral("eq_"),
                QStringLiteral("beatsync"),
                QStringLiteral("talkover"),
                QStringLiteral("main_mix"),
                QStringLiteral("mute"),
                QStringLiteral("pfl"),
                QStringLiteral("vu_meter"),
        };
        for (const QString& prefix : stripPrefixes) {
            if (key.startsWith(prefix)) {
                return true;
            }
        }
        // Stem properties live in [ChannelN_StemM] groups and are per deck
        // (R9); `stems_enabled` on the deck group must not be bridged either.
        if (key.startsWith(QLatin1String("stem"))) {
            return true;
        }
        // Transport authorities and lifecycle controls: refused on the
        // follower (5.8) or arbitrated by the manager itself (5.1, 5.7).
        static const QStringList refused = {
                QStringLiteral("sync_enabled"),
                QStringLiteral("sync_mode"),
                QStringLiteral("sync_leader"),
                QStringLiteral("sync_key"),
                QStringLiteral("vinylcontrol_enabled"),
                QStringLiteral("vinylcontrol_wantenabled"),
                QStringLiteral("eject"),
                QStringLiteral("passthrough"),
                QStringLiteral("pair_enabled"),
        };
        return refused.contains(key);
    };

    for (const QString& key : tables) {
        EXPECT_FALSE(forbidden(key))
                << "must never be bridged in either direction: "
                << key.toUtf8().constData();
    }
}

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
