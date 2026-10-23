// AI-GENERATED CONTENT DISCLAIMER: the code and comments in this file were
// generated autonomously by an AI assistant on the developer's machine, as
// allowed for assistance with code generation (see AGENTS.md). They are not an
// upstream contribution: the human developer must review them, test them on a
// real DJ setup, and submit them personally. No autonomous PRs or issues.
//
// T26 -- the marquee use case of the whole feature (DECK_PAIR_PLAN.md 1
// "Primary DJ use case", 8 T26): the instrumental plays on one member and the
// a cappella on the other, from the *same* stem file. The pair is formed, the
// vocal stem is muted on one member and soloed on the other, and both members
// then take transport edits from *both* decks for 50 callbacks.
//
// What has to hold at the same time, and what this test pins:
//   * R7  -- the pair is one deck: positions stay frame-exact through hotcue
//            and loop edits arriving from either member.
//   * R11 -- the data planes stay independent: the two members' *audio*
//            differs, because each renders a different stem subset. Equality
//            of positions does not imply this, only listening to the buffers
//            does (comparator in deckpair_test.cpp, T32).
//   * R9  -- stem properties are never shared: stem mute/solo live in
//            [ChannelN_StemM] groups and per-deck quick-effect racks, so
//            neither member's stem settings may ever move the other's.
//
// Built only with -D STEM=ON (registered in CMakeLists.txt), following the
// fixture pattern of stemcontrolobjecttest.cpp.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <tuple>

#include "control/controlobject.h"
#include "control/pollingcontrolproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "mixer/deck.h"
#include "mixer/deckpairmanager.h"
#include "test/signalpathtest.h"
#include "track/beats.h"
#include "track/track.h"

namespace {

constexpr char kPlay[] = "play";

/// The stem fixture this test runs on.
///
/// Only the AAC variant, deliberately: the ALAC_24bit stem fixtures of this
/// tree stop delivering samples after roughly half a second when played
/// through the engine (verified: a deck renders normally to about frame 20000
/// and silence from there on, for both stem files). This test compares *audio*
/// across dozens of callbacks and over seeks, so on those files every audio
/// assertion would be either vacuous (both members silent) or failing for a
/// reason that has nothing to do with deck pairs. The limitation is
/// pre-existing and unrelated to this feature -- the tree's own stem
/// control-object fixture is disabled upstream, and its enabled cases only ever
/// inspect the first few buffers. Widen this list if the ALAC decode path is
/// fixed.
const std::vector<std::string> supportedCodecs = {
        "AAC_256kbps_VBR"};

struct StemFileInfo {
    QString dir;
    QString title;
};

const std::array<StemFileInfo, 2> kStemFileInfos = {
        StemFileInfo{QStringLiteral("stem01"), QStringLiteral("sin")},
        StemFileInfo{QStringLiteral("stem02"), QStringLiteral("trance")}};

} // namespace

// must be a std::tuple for std::combine in INSTANTIATE_TEST_SUITE_P
using DeckPairStemParam = std::tuple<std::string, StemFileInfo>;

class DeckPairStemTest : public BaseSignalPathTest,
                         public ::testing::WithParamInterface<DeckPairStemParam> {
  protected:
    static constexpr int kLeader = 0;   // deck 1
    static constexpr int kFollower = 2; // deck 3
    static constexpr int kNumStems = 4;

    DeckPairStemTest() {
        // Stem sub-channels have to be registered before the track loads, as
        // in stemcontrolobjecttest.cpp: each stem is its own channel group
        // [ChannelN_StemM] with its own quick-effect rack.
        for (Deck* pDeck : {m_pMixerDeck1.get(), m_pMixerDeck3.get()}) {
            const QString group = pDeck->getGroup();
            for (int stem = 1; stem <= kNumStems; ++stem) {
                const ChannelHandleAndGroup stemGroup =
                        m_pEngineMixer->registerChannelGroup(stemGroupFor(group, stem));
                pDeck->getEngineDeck()->addStemHandle(stemGroup);
                m_pEffectsManager->addStem(stemGroup);
            }
        }

        const QString stemPath = stemFilePath();
        // One Track for both members -- the pair's premise (R11 shares the
        // transport, never the data plane; 5.7 makes both members hold the
        // leader's TrackPointer).
        m_pTrack = Track::newTemporary(stemPath);
        m_pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
                mixxx::audio::SampleRate(44100),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(120.0)));

        loadTrack(m_pMixerDeck1.get(), m_pTrack);
        loadTrack(m_pMixerDeck3.get(), m_pTrack);
        // Deck 2 is registered with the manager only to model the deck count;
        // it deliberately stays empty. A stem track on it would be rendered
        // through EngineDeck::processStem() with no stem handles registered,
        // which is out of bounds (the guard there is a DEBUG_ASSERT only).

        m_pPairManager = std::make_unique<DeckPairManager>(
                m_pConfig, m_pEngineMixer.get());
        m_pPairManager->addDeck(0, m_pMixerDeck1.get());
        m_pPairManager->addDeck(1, m_pMixerDeck2.get());
        m_pPairManager->addDeck(2, m_pMixerDeck3.get());
    }

    ~DeckPairStemTest() override {
        // The manager must go before the decks it registered.
        m_pPairManager.reset();
    }

    /// Stem loads need the Qt event loop pumped: the load completes on the
    /// caching-reader thread and `EngineDeck::slotTrackLoaded` -- which is what
    /// publishes `stem_count` and initialises the per-stem gain/mute -- runs on
    /// the main thread off `newTrackLoaded`. Same override as the stem
    /// control-object test's fixture, for the same reason.
    void loadTrack(Deck* pDeck, TrackPointer pTrack) {
        TrackPointer pLoadedTrack;
        QMetaObject::Connection connection = QObject::connect(pDeck,
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoadedTrack]( // clazy:exclude=lambda-in-connect
                        TrackPointer pNewTrack) { pLoadedTrack = pNewTrack; });
        BaseSignalPathTest::loadTrack(pDeck, pTrack);
        for (int i = 0; i < 10000 && pLoadedTrack != pTrack; ++i) {
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 1);
        }
        QObject::disconnect(connection);
        EXPECT_EQ(pTrack, pLoadedTrack)
                << "timeout loading " << pTrack->getLocation().toUtf8().constData();
        ProcessBuffer();
    }

    static QString stemGroupFor(const QString& deckGroup, int stem) {
        return deckGroup.chopped(1) + QStringLiteral("_Stem") +
                QString::number(stem) + QStringLiteral("]");
    }
    static QString stemGroup(int deckIndex, int stem) {
        return stemGroupFor(deckIndex == kLeader ? m_sGroup1 : m_sGroup3, stem);
    }
    static QString stemFxGroup(const QString& deckGroup, int stem) {
        return QStringLiteral("[QuickEffectRack1_%1]")
                .arg(stemGroupFor(deckGroup, stem));
    }

    QString stemFilePath() const {
        const auto& [codec, info] = GetParam();
        return getTestDir().filePath(QStringLiteral("stems/%1/%2_%3.stem.mp4")
                        .arg(info.dir,
                                info.title,
                                QString::fromStdString(codec)));
    }

    static QString group(int deckIndex) {
        return deckIndex == kLeader ? m_sGroup1 : m_sGroup3;
    }
    Deck* deck(int deckIndex) {
        return deckIndex == kLeader ? m_pMixerDeck1.get() : m_pMixerDeck3.get();
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

    double stemMute(int deckIndex, int stem) const {
        return ControlObject::get(ConfigKey(stemGroup(deckIndex, stem),
                QStringLiteral("mute")));
    }
    void setStemMute(int deckIndex, int stem, bool muted) {
        ControlObject::set(ConfigKey(stemGroup(deckIndex, stem),
                                   QStringLiteral("mute")),
                muted ? 1.0 : 0.0);
    }
    /// The per-deck stem quick-effect on/off, i.e. the "solo a stem on this
    /// deck" half of the use case: a rack of its own, per member.
    void setStemFxEnabled(int deckIndex, int stem, bool enabled) {
        ControlObject::set(ConfigKey(stemFxGroup(group(deckIndex), stem),
                                   QStringLiteral("enabled")),
                enabled ? 1.0 : 0.0);
    }
    double stemFxEnabled(int deckIndex, int stem) const {
        return ControlObject::get(ConfigKey(
                stemFxGroup(group(deckIndex), stem), QStringLiteral("enabled")));
    }

    double playPos(int deckIndex) {
        return buffer(deckIndex)->getPlayPos().value();
    }

    /// Same-callback frame-exact position equality plus the mirrored play
    /// state (DECK_PAIR_PLAN.md 8, assertPairLocked helper).
    void assertPairLocked() {
        EXPECT_EQ(buffer(kLeader)->getPlayPos().value(),
                buffer(kFollower)->getPlayPos().value());
        EXPECT_EQ(control(kLeader, kPlay), control(kFollower, kPlay));
    }

    /// Peak absolute sample of one member's channel buffer.
    double memberSignal(int deckIndex) const {
        const std::span<const CSAMPLE> buf = m_pEngineMixer->getChannelBuffer(
                deckIndex == kLeader ? m_sGroup1 : m_sGroup3);
        double peak = 0.0;
        for (const CSAMPLE s : buf) {
            peak = std::max(peak, std::fabs(double(s)));
        }
        return peak;
    }

    /// The worst per-sample difference between the two members' channel buffers
    /// for the callback that just ran.
    double memberAudioDelta() const {
        const std::span<const CSAMPLE> leader =
                m_pEngineMixer->getChannelBuffer(m_sGroup1);
        const std::span<const CSAMPLE> follower =
                m_pEngineMixer->getChannelBuffer(m_sGroup3);
        EXPECT_EQ(leader.size(), follower.size());
        double worst = 0.0;
        for (std::size_t i = 0; i < leader.size(); ++i) {
            worst = std::max(worst,
                    std::fabs(double(leader[i]) - double(follower[i])));
        }
        return worst;
    }

    /// Processes callbacks until both members render the same audio again.
    /// A member whose CachingReader misses a chunk writes silence but keeps
    /// its position (DECK_PAIR_PLAN.md 9.3), so after a discontinuity the
    /// audio converges a few callbacks after the position does.
    bool waitForMemberAudio(int maxCallbacks, double tolerance = 1e-6) {
        for (int i = 0; i < maxCallbacks; ++i) {
            if (memberAudioDelta() <= tolerance) {
                return true;
            }
            ProcessBuffer();
        }
        return memberAudioDelta() <= tolerance;
    }

    /// True when both members are actually rendering sound right now.
    ///
    /// Needed to read a *difference* assertion correctly: after a jump into
    /// cold data both readers can miss the same chunk in the same callback and
    /// both write silence (9.3). Two silent buffers compare equal, which means
    /// "nothing was heard", not "the pair converged" -- counting only
    /// callbacks where there was something to hear is what keeps the split
    /// assertion honest.
    bool bothMembersAudible(double threshold = 1e-4) const {
        return memberSignal(kLeader) > threshold &&
                memberSignal(kFollower) > threshold;
    }

    void expectPaired() {
        setControl(kLeader, QStringLiteral("pair_enabled"), 1.0);
        ASSERT_EQ(1.0, control(kLeader, QStringLiteral("pair_enabled")));
        ASSERT_EQ(1.0, control(kFollower, QStringLiteral("pair_enabled")));
        ASSERT_EQ(PairRole::PairLeader,
                m_pPairManager->pairRole(group(kLeader)));
        ASSERT_EQ(PairRole::PairFollower,
                m_pPairManager->pairRole(group(kFollower)));
    }

    TrackPointer m_pTrack;
    std::unique_ptr<DeckPairManager> m_pPairManager;
};

// T26: the stem split. Instrumental on one member, a cappella on the other,
// one transport, two data planes, no stem property ever crossing the pair.
TEST_P(DeckPairStemTest, StemSplitKeepsOneTransportAndTwoDataPlanes) {
    // The fixture must really be running a 4-stem file, otherwise every
    // assertion below would pass on a plain stereo track.
    ASSERT_EQ(4.0, ControlObject::get(ConfigKey(m_sGroup1, QStringLiteral("stem_count"))))
            << "the test file is not a 4-stem file: "
            << stemFilePath().toUtf8().constData();
    ASSERT_EQ(4.0, ControlObject::get(ConfigKey(m_sGroup3, QStringLiteral("stem_count"))));

    // Control for the comparator: with identical stem settings a locked pair
    // renders *identical* audio. Without this, "the members differ" below
    // could also be produced by a broken comparison.
    expectPaired();
    setControl(kLeader, kPlay, 1.0);
    ProcessBuffer();
    assertPairLocked();
    ASSERT_TRUE(waitForMemberAudio(200))
            << "identical stem settings must render identical audio";
    EXPECT_LE(memberAudioDelta(), 1e-6);
    for (int i = 0; i < 5; ++i) {
        ProcessBuffer();
        assertPairLocked();
        EXPECT_LE(memberAudioDelta(), 1e-6) << "callback " << i;
    }

    // The DJ's split: mute the vocal stem on the leader, solo it on the
    // follower (mute everything else). Stem mute is a [ChannelN_StemM] group
    // property and the solo is a per-deck quick-effect rack -- R9 says
    // neither may ever be shared.
    const int vocal = 1;
    const double leaderFxBefore = stemFxEnabled(kLeader, vocal);
    setStemMute(kLeader, vocal, true);
    for (int stem = 1; stem <= kNumStems; ++stem) {
        if (stem != vocal) {
            setStemMute(kFollower, stem, true);
        }
    }
    setStemFxEnabled(kFollower, vocal, true);
    ProcessBuffer();

    // R9: nothing crossed. The leader still unmutes the other three stems and
    // mutes only the vocal one; the follower is the mirror image, and the
    // follower's stem rack stayed the follower's own -- the leader's untouched
    // (stem FX racks are not created with a fixed 0, so compare against what
    // it was rather than assuming a value).
    EXPECT_EQ(1.0, stemMute(kLeader, vocal));
    for (int stem = 2; stem <= kNumStems; ++stem) {
        EXPECT_EQ(0.0, stemMute(kLeader, stem)) << "stem " << stem;
        EXPECT_EQ(1.0, stemMute(kFollower, stem)) << "stem " << stem;
    }
    EXPECT_EQ(0.0, stemMute(kFollower, vocal));
    EXPECT_EQ(1.0, stemFxEnabled(kFollower, vocal));
    EXPECT_EQ(leaderFxBefore, stemFxEnabled(kLeader, vocal))
            << "the follower's stem solo reached the leader's stem rack";

    // The split is audible: the members now render different audio, while
    // remaining one deck. This is R11 -- two independent data planes reading
    // the same Track, each decoding and mixing its own stem subset. Wait for a
    // callback where both members are actually producing sound first (a
    // simultaneous reader cache miss leaves both silent, 9.3).
    bool sawSplit = false;
    for (int i = 0; i < 40 && !sawSplit; ++i) {
        ProcessBuffer();
        assertPairLocked();
        if (bothMembersAudible()) {
            sawSplit = memberAudioDelta() > 1e-4;
        }
    }
    EXPECT_TRUE(sawSplit)
            << "the members never rendered different audio, so the stem "
               "split never reached the audio plane";

    // R7: 50 callbacks of hotcue and loop edits arriving from *both* decks.
    // Every callback must stay frame-exact. Where both members actually
    // rendered sound, the audio must still differ: the split has to survive
    // every transport edit, and no edit may "heal" it by routing one member's
    // stem settings through the pair.
    int audibleCallbacks = 0;
    int splitCallbacks = 0;
    for (int i = 0; i < 50; ++i) {
        switch (i) {
        case 0:
            // Hotcue points from each side, then fire them from the other.
            pulse(kLeader, QStringLiteral("hotcue_1_set"));
            pulse(kFollower, QStringLiteral("hotcue_2_set"));
            break;
        case 8:
            pulse(kFollower, QStringLiteral("hotcue_1_goto"));
            break;
        case 16:
            pulse(kLeader, QStringLiteral("hotcue_2_goto"));
            break;
        case 24:
            // A loop engaged on the follower, resized on the leader.
            setControl(kFollower, QStringLiteral("beatloop_size"), 4.0);
            pulse(kFollower, QStringLiteral("beatloop_4_activate"));
            break;
        case 36:
            pulse(kLeader, QStringLiteral("loop_halve"));
            break;
        default:
            break;
        }
        ProcessBuffer();
        assertPairLocked();
        if (!bothMembersAudible()) {
            continue; // both readers missed the same chunk: nothing to compare
        }
        ++audibleCallbacks;
        if (memberAudioDelta() > 1e-4) {
            ++splitCallbacks;
        } else {
            ADD_FAILURE() << "callback " << i
                          << ": both members rendered sound and it was the same "
                             "sound, although their stem settings differ -- a stem "
                             "property must have crossed the pair";
        }
    }
    // The run must have actually been audible for long enough that the
    // assertion above is not vacuous.
    EXPECT_GE(audibleCallbacks, 25)
            << "the pair was silent for most of the run, so the split "
               "assertion proved nothing";
    EXPECT_EQ(audibleCallbacks, splitCallbacks);

    // The edits really happened, on the pair (not on one member only): one
    // loop region and two hotcue points, identical on both members.
    EXPECT_EQ(1.0, control(kLeader, QStringLiteral("loop_enabled")));
    EXPECT_EQ(1.0, control(kFollower, QStringLiteral("loop_enabled")));
    EXPECT_EQ(control(kLeader, QStringLiteral("loop_start_position")),
            control(kFollower, QStringLiteral("loop_start_position")));
    EXPECT_EQ(control(kLeader, QStringLiteral("loop_end_position")),
            control(kFollower, QStringLiteral("loop_end_position")));
    for (const char* item : {"hotcue_1_status", "hotcue_2_status"}) {
        EXPECT_EQ(1.0, control(kLeader, QString::fromLatin1(item)));
        EXPECT_EQ(1.0, control(kFollower, QString::fromLatin1(item)));
        EXPECT_EQ(control(kLeader, QString::fromLatin1(item)),
                control(kFollower, QString::fromLatin1(item)));
    }

    // And the pair is still audibly one deck once the split is undone: same
    // stem settings again, so the two independent data planes must produce
    // the same stream again, sample for sample. This is the proof that the
    // difference measured above came from the stems and not from a desync.
    for (int stem = 1; stem <= kNumStems; ++stem) {
        setStemMute(kLeader, stem, false);
        setStemMute(kFollower, stem, false);
    }
    setStemFxEnabled(kFollower, vocal, false);
    EXPECT_TRUE(waitForMemberAudio(200))
            << "with identical stem settings the members must render "
               "identical audio again";
    EXPECT_LE(memberAudioDelta(), 1e-6);
    assertPairLocked();
}

INSTANTIATE_TEST_SUITE_P(
        Codecs,
        DeckPairStemTest,
        ::testing::Combine(::testing::ValuesIn(supportedCodecs),
                ::testing::ValuesIn(kStemFileInfos)),
        [](const ::testing::TestParamInfo<DeckPairStemTest::ParamType>& info)
                -> std::string {
            // std::get, not a structured binding: a comma inside braces would
            // split the macro argument list.
            const std::string codec = std::get<0>(info.param);
            const std::string title =
                    std::get<1>(info.param).title.toStdString();
            std::string name = codec;
            std::replace(name.begin(), name.end(), '-', '_');
            return name + '_' + title;
        });

// End of autonomously AI-generated content; see the disclaimer at the top of
// this file.
