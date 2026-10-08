#include <gtest/gtest.h>

#include <QScopedPointer>
#include <QtDebug>
#include <cmath>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "engine/channels/enginedecksecondary.h"
#include "gtest/gtest.h"
#include "mixxxtest.h"
#include "test/signalpathtest.h"

#define STEM_FILE QStringLiteral("stems/sin_%1.stem.mp4").arg(QString::fromStdString(GetParam()))

namespace {
const std::vector<std::string> supportedCodecs = {
        "AAC_256kbps_VBR",
        "ALAC_24bit"};
} // namespace

class StemControlFixture : public BaseSignalPathTest,
                           public ::testing::WithParamInterface<std::string> {
  public:
    QString getGroupForStem(QStringView deckGroup, int stemNr) {
        DEBUG_ASSERT(deckGroup.endsWith(QChar(']')) && stemNr <= 4);
        return deckGroup.chopped(1) + QStringLiteral("_Stem") + QChar('0' + stemNr) + QChar(']');
    }
    QString getFxGroupForStem(const QString& deckGroup, int stemNr) {
        return QStringLiteral("[QuickEffectRack1_%1]")
                .arg(getGroupForStem(deckGroup, stemNr));
    }
    QString getGroupForSecondary(QStringView deckGroup) {
        DEBUG_ASSERT(deckGroup.endsWith(QChar(']')));
        return deckGroup.chopped(1) + QStringLiteral("Secondary]");
    }

    void addSecondaryChannel(EngineDeck* pPrimaryDeck, const QString& primaryGroup) {
        ChannelHandleAndGroup secondaryHandleGroup =
                m_pEngineMixer->registerChannelGroup(getGroupForSecondary(primaryGroup));
        auto pSecondary = std::make_unique<EngineDeckSecondary>(
                secondaryHandleGroup,
                pPrimaryDeck,
                m_pEffectsManager.get(),
                EngineChannel::CENTER);
        pSecondary->setMainMix(true);
        m_pEngineMixer->addChannel(std::move(pSecondary));
        m_pEffectsManager->addDeck(secondaryHandleGroup);
    }

    // Seek deck 1 to frame 0 and process enough buffers for gain ramps to
    // settle.
    void seekAndSettle(int numProcesses = 3) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
        for (int i = 0; i < numProcesses; i++) {
            m_pEngineMixer->process(kProcessBufferSize);
        }
    }

    double bufferRms(std::span<const CSAMPLE> buffer) const {
        if (buffer.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (CSAMPLE sample : buffer) {
            sum += static_cast<double>(sample) * sample;
        }
        return std::sqrt(sum / static_cast<double>(buffer.size()));
    }

    void expectBufferNear(std::span<const CSAMPLE> expected,
            std::span<const CSAMPLE> actual,
            double delta = 0.0001) const {
        ASSERT_EQ(expected.size(), actual.size());
        double maxDiff = 0.0;
        for (std::size_t i = 0; i < expected.size(); i++) {
            const double diff = std::fabs(
                    static_cast<double>(expected[i]) - static_cast<double>(actual[i]));
            if (diff > maxDiff) {
                maxDiff = diff;
            }
        }
        EXPECT_LE(maxDiff, delta) << "Max sample difference " << maxDiff;
    }

    // Copies the buffer so it can be compared after further processing.
    std::vector<CSAMPLE> captureBuffer(std::span<const CSAMPLE> buffer) const {
        return std::vector<CSAMPLE>(buffer.begin(), buffer.end());
    }

    // Element-wise sum of two equal sized buffers.
    static std::vector<CSAMPLE> addBuffers(
            const std::vector<CSAMPLE>& a, const std::vector<CSAMPLE>& b) {
        std::vector<CSAMPLE> sum(a.size());
        for (std::size_t i = 0; i < a.size(); i++) {
            sum[i] = a[i] + b[i];
        }
        return sum;
    }

    void SetUp() override {
        BaseSignalPathTest::SetUp();

        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup1, i));
            m_pChannel1->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup2, i));
            m_pChannel2->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup3, i));
            m_pChannel3->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }

        // Create the secondary channels the same way
        // PlayerManager::addDeckInner() does.
        addSecondaryChannel(m_pChannel1, m_sGroup1);
        addSecondaryChannel(m_pChannel2, m_sGroup2);
        addSecondaryChannel(m_pChannel3, m_sGroup3);

        const QString kStemFileLocationTest = getTestDir().filePath(STEM_FILE);
        TrackPointer pStemFile(Track::newTemporary(kStemFileLocationTest));

        loadTrack(m_pMixerDeck1.get(), pStemFile);
        loadTrack(m_pMixerDeck3.get(), pStemFile);

        m_pPlay = std::make_unique<PollingControlProxy>(m_sGroup1, "play");

        m_pStem1Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "volume");
        m_pStem2Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "volume");
        m_pStem3Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "volume");
        m_pStem4Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "volume");
        m_pStem1Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 1), "mute");
        m_pStem2Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 2), "mute");
        m_pStem3Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 3), "mute");
        m_pStem4Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 4), "mute");
        m_pStem1AltChannel = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "alt_channel");
        m_pStem2AltChannel = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "alt_channel");
        m_pStem3AltChannel = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "alt_channel");
        m_pStem4AltChannel = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "alt_channel");
        m_pStem1Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "color");
        m_pStem2Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "color");
        m_pStem3Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "color");
        m_pStem4Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "color");
        m_pStem1FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 1), "enabled");
        m_pStem2FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 2), "enabled");
        m_pStem3FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 3), "enabled");
        m_pStem4FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 4), "enabled");

        m_pStem1FXEnabled->set(0.0);
        m_pStem2FXEnabled->set(0.0);
        m_pStem3FXEnabled->set(0.0);
        m_pStem4FXEnabled->set(0.0);

        m_pStem1VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "vu_meter");
        m_pStem2VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "vu_meter");
        m_pStem3VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "vu_meter");
        m_pStem4VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "vu_meter");

        m_pStemCount = std::make_unique<PollingControlProxy>(m_sGroup1, "stem_count");
    }

    void setCurrentPosition(mixxx::audio::FramePos position) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(position, EngineBuffer::SEEK_STANDARD);
        ProcessBuffer();
    }

    void loadTrack(Deck* pDeck, TrackPointer pTrack) {
        // Because there is connection across the main thread in caching reader
        // thread, we need to manually process the Qt event loop to trigger
        // `BaseTrackPlayerImpl::slotTrackLoaded` Here is the chain of
        // connections (Symbol (thread)) EngineDeck::slotLoadTrack (main) ->
        // EngineBuffer::loadTrack (main) -> CachingReader*::newTrack (main) ->
        // CachingReaderWorker::trackLoaded (CachingReader) ->
        // EngineBuffer::loaded (CachingReader, direct) ->
        // BaseTrackPlayerImpl::slotTrackLoaded  (main)

        TrackPointer pLoadedTrack;
        QMetaObject::Connection connection = QObject::connect(pDeck,
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoadedTrack]( // clazy:exclude=lambda-in-connect
                        TrackPointer pNewTrack) { pLoadedTrack = pNewTrack; });
        BaseSignalPathTest::loadTrack(pDeck, pTrack);

        for (int i = 0; i < 10000; ++i) {
            if (pLoadedTrack == pTrack) {
                break;
            }
            int maxtime = 1; // ms
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, maxtime);
            // 1 ms for waiting 10 s at max
        }
        QObject::disconnect(connection);
        if (pLoadedTrack != pTrack) {
            qWarning() << "Timeout: failed loading track" << pTrack->getLocation();
        }
    }

    std::unique_ptr<PollingControlProxy> m_pPlay;
    std::unique_ptr<PollingControlProxy> m_pStem1Volume;
    std::unique_ptr<PollingControlProxy> m_pStem2Volume;
    std::unique_ptr<PollingControlProxy> m_pStem3Volume;
    std::unique_ptr<PollingControlProxy> m_pStem4Volume;
    std::unique_ptr<PollingControlProxy> m_pStem1Mute;
    std::unique_ptr<PollingControlProxy> m_pStem2Mute;
    std::unique_ptr<PollingControlProxy> m_pStem3Mute;
    std::unique_ptr<PollingControlProxy> m_pStem4Mute;
    std::unique_ptr<PollingControlProxy> m_pStem1AltChannel;
    std::unique_ptr<PollingControlProxy> m_pStem2AltChannel;
    std::unique_ptr<PollingControlProxy> m_pStem3AltChannel;
    std::unique_ptr<PollingControlProxy> m_pStem4AltChannel;
    std::unique_ptr<PollingControlProxy> m_pStem1Color;
    std::unique_ptr<PollingControlProxy> m_pStem2Color;
    std::unique_ptr<PollingControlProxy> m_pStem3Color;
    std::unique_ptr<PollingControlProxy> m_pStem4Color;
    std::unique_ptr<PollingControlProxy> m_pStem1FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem2FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem3FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem4FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem1VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem2VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem3VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem4VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStemCount;
};

TEST_P(StemControlFixture, StemCount) {
    EXPECT_EQ(m_pStemCount->get(), 4.0);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStemCount->get(), 0.0);

    kTrackLocationTest = getTestDir().filePath(STEM_FILE);
    pTrack = Track::newTemporary(kTrackLocationTest);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStemCount->get(), 4.0);
}

TEST_P(StemControlFixture, StemColor) {
    EXPECT_EQ(m_pStem1Color->get(), 0xfd << 16 | 0x4a << 8 | 0x4a);
    EXPECT_EQ(m_pStem2Color->get(), 0xff << 16 | 0xff << 8 | 0x00);
    EXPECT_EQ(m_pStem3Color->get(), 0x00 << 16 | 0xe8 << 8 | 0xe8);
    EXPECT_EQ(m_pStem4Color->get(), 0xad << 16 | 0x65 << 8 | 0xff);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Color->get(), -1.0);
    EXPECT_EQ(m_pStem2Color->get(), -1.0);
    EXPECT_EQ(m_pStem3Color->get(), -1.0);
    EXPECT_EQ(m_pStem4Color->get(), -1.0);

    kTrackLocationTest = getTestDir().filePath(STEM_FILE);
    pTrack = Track::newTemporary(kTrackLocationTest);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Color->get(), 0xfd << 16 | 0x4a << 8 | 0x4a);
    EXPECT_EQ(m_pStem2Color->get(), 0xff << 16 | 0xff << 8 | 0x00);
    EXPECT_EQ(m_pStem3Color->get(), 0x00 << 16 | 0xe8 << 8 | 0xe8);
    EXPECT_EQ(m_pStem4Color->get(), 0xad << 16 | 0x65 << 8 | 0xff);
}

TEST_P(StemControlFixture, Volume) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    m_pStem1Volume->set(0.0);
    m_pStem2Volume->set(0.0);
    m_pStem3Volume->set(0.0);
    m_pStem4Volume->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlSilence"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Volume->set(1.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumOnly"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem2Volume->set(0.8);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumAndBass"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Volume->set(0.5);
    m_pStem3Volume->set(0.2);
    m_pStem4Volume->set(0.4);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlFull"));
}

TEST_P(StemControlFixture, VolumeResetOnLoad) {
    m_pStem1Volume->set(0.1);
    m_pStem2Volume->set(0.2);
    m_pStem3Volume->set(0.3);
    m_pStem4Volume->set(0.4);
    m_pStem1Mute->set(1.0);
    m_pStem2Mute->set(1.0);
    m_pStem3Mute->set(0.0);
    m_pStem4Mute->set(1.0);
    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), false);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Volume->get(), 0.1);
    EXPECT_EQ(m_pStem2Volume->get(), 0.2);
    EXPECT_EQ(m_pStem3Volume->get(), 0.3);
    EXPECT_EQ(m_pStem4Volume->get(), 0.4);
    EXPECT_EQ(m_pStem1Mute->get(), 1.0);
    EXPECT_EQ(m_pStem2Mute->get(), 1.0);
    EXPECT_EQ(m_pStem3Mute->get(), 0.0);
    EXPECT_EQ(m_pStem4Mute->get(), 1.0);

    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), true);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Volume->get(), 1.0);
    EXPECT_EQ(m_pStem2Volume->get(), 1.0);
    EXPECT_EQ(m_pStem3Volume->get(), 1.0);
    EXPECT_EQ(m_pStem4Volume->get(), 1.0);
    EXPECT_EQ(m_pStem1Mute->get(), 0.0);
    EXPECT_EQ(m_pStem2Mute->get(), 0.0);
    EXPECT_EQ(m_pStem3Mute->get(), 0.0);
    EXPECT_EQ(m_pStem4Mute->get(), 0.0);
}

TEST_P(StemControlFixture, Mute) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    m_pStem1Mute->set(1.0);
    m_pStem2Mute->set(1.0);
    m_pStem3Mute->set(1.0);
    m_pStem4Mute->set(1.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlSilence")); // Same than volume test

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Mute->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumOnly")); // Same than volume test

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem2Mute->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemMuteControlDrumAndBass"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem3Mute->set(0.0);
    m_pStem4Mute->set(0.0);

    // We need to allow a bigger bigger delta than the 0.0001 default
    // to cover the difference of different AAC decoder.
    // aac and libfdk_aac have a difference of 0.00017 in tests with FFmpeg 4.4.2
    double acceptableDelta = 0.0002; // -74 dB
    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemMuteControlFull"),
            acceptableDelta);
}

TEST_P(StemControlFixture, VuMeter) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);

    // Initial check: silence
    EXPECT_EQ(m_pStem1VuMeter->get(), 0.0);

    // Process buffer to play sound
    // Run enough cycles to trigger VU meter update (30Hz update rate vs ~44kHz/buffer)
    for (int i = 0; i < 50; ++i) {
        m_pEngineMixer->process(kProcessBufferSize);
    }

    // Check if VU meters picked up the signal
    EXPECT_GT(m_pStem1VuMeter->get(), 0.0);
    EXPECT_GT(m_pStem2VuMeter->get(), 0.0);

    // Mute Stem 1
    m_pStem1Mute->set(1.0);

    // Process enough buffers to allow VU meter to decay to 0
    // Decay is exponential, so it takes time.
    for (int i = 0; i < 600; ++i) {
        m_pEngineMixer->process(kProcessBufferSize);
    }

    // VU Meter should be near zero (allow small epsilon for imperfect decay)
    EXPECT_NEAR(m_pStem1VuMeter->get(), 0.0, 0.001);

    // Stem 2 should still be playing
    EXPECT_GT(m_pStem2VuMeter->get(), 0.0);
}

TEST_P(StemControlFixture, AltRouting) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    seekAndSettle();

    const QString secondaryGroup = getGroupForSecondary(m_sGroup1);

    // All stems on the primary channel: the secondary channel is silent.
    // Compare the primary channel buffer (not the main mix, which also
    // contains the secondary channel).
    const std::vector<CSAMPLE> reference =
            captureBuffer(m_pEngineMixer->getChannelBuffer(m_sGroup1));
    EXPECT_GT(bufferRms(reference), 0.01);
    const std::vector<CSAMPLE> secondarySilent =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_NEAR(bufferRms(secondarySilent), 0.0, 0.001);

    // Route stem 1 to the secondary channel.
    m_pStem1AltChannel->set(1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> primaryOnly =
            captureBuffer(m_pEngineMixer->getChannelBuffer(m_sGroup1));
    const std::vector<CSAMPLE> secondaryStem1 =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_GT(bufferRms(secondaryStem1), 0.01);

    // The stem is neither duplicated nor lost: primary + secondary
    // equals the original mix.
    expectBufferNear(
            reference, addBuffers(primaryOnly, secondaryStem1));
}

TEST_P(StemControlFixture, AllStemsToSecondary) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    seekAndSettle();

    const QString secondaryGroup = getGroupForSecondary(m_sGroup1);
    const std::vector<CSAMPLE> reference =
            captureBuffer(m_pEngineMixer->getChannelBuffer(m_sGroup1));
    EXPECT_GT(bufferRms(reference), 0.01);

    // Route all stems to the secondary channel.
    m_pStem1AltChannel->set(1.0);
    m_pStem2AltChannel->set(1.0);
    m_pStem3AltChannel->set(1.0);
    m_pStem4AltChannel->set(1.0);
    seekAndSettle();

    // The primary channel is silent and the secondary channel carries
    // the full mix.
    const std::vector<CSAMPLE> primaryEmpty =
            captureBuffer(m_pEngineMixer->getChannelBuffer(m_sGroup1));
    EXPECT_NEAR(bufferRms(primaryEmpty), 0.0, 0.001);
    const std::vector<CSAMPLE> secondaryAll =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    expectBufferNear(reference, secondaryAll);
}

TEST_P(StemControlFixture, AltRoutingResetOnLoad) {
    m_pStem1AltChannel->set(1.0);
    m_pStem2AltChannel->set(1.0);
    m_pStem3AltChannel->set(1.0);
    m_pStem4AltChannel->set(1.0);
    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), false);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1AltChannel->get(), 1.0);
    EXPECT_EQ(m_pStem2AltChannel->get(), 1.0);
    EXPECT_EQ(m_pStem3AltChannel->get(), 1.0);
    EXPECT_EQ(m_pStem4AltChannel->get(), 1.0);

    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), true);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1AltChannel->get(), 0.0);
    EXPECT_EQ(m_pStem2AltChannel->get(), 0.0);
    EXPECT_EQ(m_pStem3AltChannel->get(), 0.0);
    EXPECT_EQ(m_pStem4AltChannel->get(), 0.0);
}

TEST_P(StemControlFixture, AltRoutingKeepsMute) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    seekAndSettle();

    const QString secondaryGroup = getGroupForSecondary(m_sGroup1);

    // Mute stem 1 and route it to the secondary channel: it stays muted.
    m_pStem1Mute->set(1.0);
    m_pStem1AltChannel->set(1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> secondaryMuted =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_NEAR(bufferRms(secondaryMuted), 0.0, 0.001);

    // Unmute it: the stem now reaches the secondary channel.
    m_pStem1Mute->set(0.0);
    seekAndSettle();

    const std::vector<CSAMPLE> secondaryUnmuted =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_GT(bufferRms(secondaryUnmuted), 0.01);
}

TEST_P(StemControlFixture, SecondaryChannelXfaderRouting) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    seekAndSettle();

    const QString secondaryGroup = getGroupForSecondary(m_sGroup1);

    // Route stem 1 to the secondary channel and point the primary channel
    // to the left bus and the secondary channel to the right bus.
    m_pStem1AltChannel->set(1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "orientation_left"), 1.0);
    ControlObject::set(ConfigKey(secondaryGroup, "orientation_right"), 1.0);
    seekAndSettle();

    // Crossfader hard left: only the primary channel reaches the main mix,
    // the routed stem is not duplicated into it.
    const double crossfaderOld =
            ControlObject::get(ConfigKey(m_sMainGroup, "crossfader"));
    ControlObject::set(ConfigKey(m_sMainGroup, "crossfader"), -1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> primaryBuffer =
            captureBuffer(m_pEngineMixer->getChannelBuffer(m_sGroup1));
    const std::vector<CSAMPLE> mainLeft =
            captureBuffer(m_pEngineMixer->getMainBuffer());
    expectBufferNear(primaryBuffer, mainLeft);

    // Crossfader hard right: only the secondary channel reaches the
    // main mix.
    ControlObject::set(ConfigKey(m_sMainGroup, "crossfader"), 1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> secondaryBuffer =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_GT(bufferRms(secondaryBuffer), 0.01);
    const std::vector<CSAMPLE> mainRight =
            captureBuffer(m_pEngineMixer->getMainBuffer());
    expectBufferNear(secondaryBuffer, mainRight);

    // Restore the control state so it does not leak into other tests.
    ControlObject::set(ConfigKey(m_sMainGroup, "crossfader"), crossfaderOld);
    ControlObject::set(ConfigKey(m_sGroup1, "orientation_center"), 1.0);
    ControlObject::set(ConfigKey(secondaryGroup, "orientation_center"), 1.0);
}

TEST_P(StemControlFixture, SecondaryChannelPfl) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    seekAndSettle();

    const QString secondaryGroup = getGroupForSecondary(m_sGroup1);

    // Route stem 1 to the secondary channel.
    m_pStem1AltChannel->set(1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> secondaryBuffer =
            captureBuffer(m_pEngineMixer->getChannelBuffer(secondaryGroup));
    EXPECT_GT(bufferRms(secondaryBuffer), 0.01);

    // Enable PFL on the secondary channel: its audio shows up in the
    // headphone mix (pre-fader, so without the channel fader gain).
    ControlObject::set(ConfigKey(secondaryGroup, "pfl"), 1.0);
    seekAndSettle();

    const std::vector<CSAMPLE> headphones =
            captureBuffer(m_pEngineMixer->getHeadphoneBuffer());
    // The default headMix (-1) gives a PFL gain of 1.
    expectBufferNear(secondaryBuffer, headphones);

    // Restore the control state so it does not leak into other tests.
    ControlObject::set(ConfigKey(secondaryGroup, "pfl"), 0.0);
}

INSTANTIATE_TEST_SUITE_P(
        DISABLED_StemControlTest,
        StemControlFixture,
        ::testing::ValuesIn(supportedCodecs),
        [](const testing::TestParamInfo<StemControlFixture::ParamType>& info) {
            return info.param;
        });
