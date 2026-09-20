#pragma once

#include <memory>

#include <QObject>

#include "mixer/basetrackplayer.h"

class StemControls;

class Deck : public BaseTrackPlayerImpl {
    Q_OBJECT
  public:
    Deck(PlayerManager* pParent,
            UserSettingsPointer pConfig,
            EngineMixer* pMixingEngine,
            EffectsManager* pEffectsManager,
            EngineChannel::ChannelOrientation defaultOrientation,
            const ChannelHandleAndGroup& handleGroup);
    ~Deck() override;

  private:
    // Bite DJ: the [ChannelN],stem_* controls for pre-separated stems. Decks
    // only — a sampler has no use for them. See stemcontrols.h.
    std::unique_ptr<StemControls> m_pStemControls;
};
