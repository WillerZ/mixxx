#include "engine/channels/enginedecksecondary.h"

#ifdef __STEM__

#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/effects/engineeffectsmanager.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/enginebuffer.h"
#include "moc_enginedecksecondary.cpp"
#include "util/sample.h"

EngineDeckSecondary::EngineDeckSecondary(
        const ChannelHandleAndGroup& handleGroup,
        EngineDeck* pPrimaryDeck,
        EffectsManager* pEffectsManager,
        EngineChannel::ChannelOrientation defaultOrientation)
        : EngineChannel(handleGroup, defaultOrientation, pEffectsManager,
                  /*isTalkoverChannel*/ false,
                  /*isPrimaryDeck*/ false),
          m_pPrimaryDeck(pPrimaryDeck) {
}

EngineDeckSecondary::~EngineDeckSecondary() = default;

void EngineDeckSecondary::process(CSAMPLE* pOutput, const std::size_t bufferSize) {
    // Copy the stems routed to the secondary channel, produced by the primary
    // deck earlier in this engine callback.
    SampleUtil::copy(pOutput, m_pPrimaryDeck->altOutput(), bufferSize);

    // Apply the prefader effects (EQ chain) of this channel.
    EngineEffectsManager* pEngineEffectsManager = m_pEffectsManager->getEngineEffectsManager();
    if (pEngineEffectsManager != nullptr) {
        pEngineEffectsManager->processPreFaderInPlace(m_group.handle(),
                m_pEffectsManager->getMainHandle(),
                pOutput,
                bufferSize,
                mixxx::audio::SampleRate::fromDouble(m_sampleRate.get()));
    }

    // Update VU meter
    m_vuMeter.process(pOutput, bufferSize);
}

void EngineDeckSecondary::collectFeatures(GroupFeatureState* pGroupFeatures) const {
    // Beat and tempo features are inherited from the primary deck so that the
    // quick effects (e.g. beat FX) of this channel stay synchronized with it.
    m_pPrimaryDeck->collectFeatures(pGroupFeatures);
}

EngineChannel::ActiveState EngineDeckSecondary::updateActiveState() {
    EngineBuffer* pPrimaryBuffer = m_pPrimaryDeck->getEngineBuffer();
    bool active = pPrimaryBuffer != nullptr && pPrimaryBuffer->isTrackLoaded();

    if (active) {
        m_active = true;
        return ActiveState::Active;
    }
    if (m_active) {
        m_vuMeter.reset();
        m_active = false;
        return ActiveState::WasActive;
    }
    return ActiveState::Inactive;
}

#endif
