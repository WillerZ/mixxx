#pragma once

#include "engine/channels/enginechannel.h"

class EngineDeck;
struct GroupFeatureState;

#ifdef __STEM__
/// A mixer channel that carries the stems of a primary EngineDeck that are
/// routed to the secondary channel (see EngineDeck::processStem()). It has no
/// EngineBuffer of its own and therefore can never be a sync target: the
/// audio is produced by the primary deck earlier in the same engine callback
/// and copied here, so it flows through the standard mixer path and gets its
/// own fader, mute, PFL, crossfader orientation, VU meter, EQ and quick FX.
class EngineDeckSecondary : public EngineChannel {
    Q_OBJECT
  public:
    EngineDeckSecondary(const ChannelHandleAndGroup& handleGroup,
            EngineDeck* pPrimaryDeck,
            EffectsManager* pEffectsManager,
            EngineChannel::ChannelOrientation defaultOrientation);
    ~EngineDeckSecondary() override;

    void process(CSAMPLE* pOutput, const std::size_t bufferSize) override;
    void collectFeatures(GroupFeatureState* pGroupFeatures) const override;
    EngineChannel::ActiveState updateActiveState() override;

  private:
    // Not owned. Must outlive this channel.
    EngineDeck* m_pPrimaryDeck;
};
#endif
