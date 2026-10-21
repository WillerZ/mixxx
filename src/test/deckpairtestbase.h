// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
#pragma once

#include <gtest/gtest.h>

#include <QString>
#include <QTest>
#include <memory>

#include "control/controlobject.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "mixer/deck.h"
#include "mixer/deckpairmanager.h"
#include "test/mockedenginebackendtest.h"
#include "track/beats.h"
#include "track/track.h"

/// Fixture for the deck pair tests.
///
/// Extends MockedEngineBackendTest with a fourth deck (pairs are decks 1+3 and
/// 2+4, so three decks are not enough), replaces the fake tracks of the base
/// fixture by real, loadable ones -- track adoption runs through the regular
/// load path -- and owns a DeckPairManager.
///
/// The manager starts out without any registered deck: tests call
/// registerDecks() to model PlayerManager creating decks 1..n. That is what
/// makes "the partner deck does not exist yet" (DECK_PAIR_PLAN.md R2/T4)
/// testable without a PlayerManager.
class DeckPairTestBase : public MockedEngineBackendTest {
  protected:
    DeckPairTestBase() {
        m_pMixerDeck4 = std::make_unique<Deck>(nullptr,
                m_pConfig,
                m_pEngineMixer.get(),
                m_pEffectsManager.get(),
                EngineChannel::CENTER,
                m_pEngineMixer->registerChannelGroup(kGroup4));
        m_pChannel4 = m_pMixerDeck4->getEngineDeck();
        m_pMockScaleVinyl4 = new MockScaler();
        m_pMockScaleKeylock4 = new MockScaler();
        m_pChannel4->getEngineBuffer()->setScalerForTest(
                m_pMockScaleVinyl4, m_pMockScaleKeylock4);
        addDeck(m_pChannel4);

        // Real tracks: three distinct Track objects over the same audio file,
        // which is the deck pair use case (same track on both members).
        const QString trackLocation =
                getTestDir().filePath(QStringLiteral("sine-30.wav"));
        m_pTrackA = Track::newTemporary(trackLocation);
        m_pTrackB = Track::newTemporary(trackLocation);
        m_pTrackC = Track::newTemporary(trackLocation);

        // A constant beat grid makes quantize, beatloops and beatjumps
        // meaningful for the pair tests. sine-30.wav is 44100 Hz; the grid is
        // attached before the decks load so the engine controls see it.
        attachBeatGrid(m_pTrackA);
        attachBeatGrid(m_pTrackB);
        attachBeatGrid(m_pTrackC);

        loadTrack(m_pMixerDeck1.get(), m_pTrackA);
        loadTrack(m_pMixerDeck2.get(), m_pTrackA);
        loadTrack(m_pMixerDeck3.get(), m_pTrackB);
        loadTrack(m_pMixerDeck4.get(), m_pTrackB);

        // Owned by the fixture, and therefore destroyed before the decks of
        // the base class: the manager disarms all pairs on its way out.
        m_pPairManager = std::make_unique<DeckPairManager>(
                m_pConfig, m_pEngineMixer.get());
    }

    ~DeckPairTestBase() override {
        // The manager must go before the decks it registered.
        m_pPairManager.reset();
        delete m_pMockScaleVinyl4;
        delete m_pMockScaleKeylock4;
    }

    /// Registers decks 0..count-1 with the manager, like PlayerManager does
    /// while it grows the deck count.
    void registerDecks(int count) {
        for (int i = 0; i < count; ++i) {
            m_pPairManager->addDeck(i, deck(i));
        }
    }

    static void attachBeatGrid(const TrackPointer& pTrack) {
        pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
                mixxx::audio::SampleRate(44100),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(120.0)));
    }

    /// Deck index is 0-based, groups are [Channel1] .. [Channel4].
    static QString group(int deckIndex) {
        switch (deckIndex) {
        case 0:
            return m_sGroup1;
        case 1:
            return m_sGroup2;
        case 2:
            return m_sGroup3;
        case 3:
            return kGroup4;
        default:
            return QString();
        }
    }

    Deck* deck(int deckIndex) {
        switch (deckIndex) {
        case 0:
            return m_pMixerDeck1.get();
        case 1:
            return m_pMixerDeck2.get();
        case 2:
            return m_pMixerDeck3.get();
        case 3:
            return m_pMixerDeck4.get();
        default:
            return nullptr;
        }
    }

    EngineBuffer* buffer(int deckIndex) {
        Deck* pDeck = deck(deckIndex);
        return pDeck ? pDeck->getEngineDeck()->getEngineBuffer() : nullptr;
    }

    double control(int deckIndex, const QString& item) const {
        return ControlObject::get(ConfigKey(group(deckIndex), item));
    }

    void setControl(int deckIndex, const QString& item, double value) {
        ControlObject::set(ConfigKey(group(deckIndex), item), value);
    }

    double pairEnabled(int deckIndex) const {
        return control(deckIndex, QStringLiteral("pair_enabled"));
    }

    void requestPair(int deckIndex) {
        setControl(deckIndex, QStringLiteral("pair_enabled"), 1.0);
    }

    void requestUnpair(int deckIndex) {
        setControl(deckIndex, QStringLiteral("pair_enabled"), 0.0);
    }

    void setPlaying(int deckIndex, bool playing) {
        setControl(deckIndex, QStringLiteral("play"), playing ? 1.0 : 0.0);
        ProcessBuffer();
    }

    /// Pairs deckIndex with its partner and asserts that it worked.
    void expectPaired(int leaderIndex) {
        const int followerIndex = DeckPairManager::partnerDeckIndex(leaderIndex);
        ASSERT_GE(followerIndex, 0);
        requestPair(leaderIndex);
        EXPECT_EQ(1.0, pairEnabled(leaderIndex));
        EXPECT_EQ(1.0, pairEnabled(followerIndex));
        EXPECT_EQ(PairRole::PairLeader, m_pPairManager->pairRole(group(leaderIndex)));
        EXPECT_EQ(PairRole::PairFollower,
                m_pPairManager->pairRole(group(followerIndex)));
    }

    /// Processes buffers until the engine side of \p deckIndex works on
    /// \p pTrack, or gives up after ~2 s.
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

    inline static const QString kGroup4 = QStringLiteral("[Channel4]");

    std::unique_ptr<Deck> m_pMixerDeck4;
    EngineDeck* m_pChannel4{nullptr};
    MockScaler* m_pMockScaleVinyl4{nullptr};
    MockScaler* m_pMockScaleKeylock4{nullptr};
    TrackPointer m_pTrackA;
    TrackPointer m_pTrackB;
    TrackPointer m_pTrackC;
    std::unique_ptr<DeckPairManager> m_pPairManager;
};

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
