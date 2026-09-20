#pragma once

#include <memory>
#include <vector>

#include "engine/cachingreader/cachingreader.h"
#include "preferences/usersettings.h"

class ControlObject;
class ControlPushButton;

/// Bite DJ: a deck reader that plays a track's two stems instead of the track.
///
/// When a track has pre-separated stems beside it on the drive (see StemSet),
/// this reads the vocal and instrumental files instead of the original and
/// sums them as the engine pulls samples. Both enabled is the track as
/// recorded; dropping one leaves the other playing.
///
/// **Where the mixing happens is the whole design.** This sits behind
/// `ReadAheadManager`, so the sum happens after the cache and *before* the
/// scaler: one timestretcher per deck rather than one per stem. Keylock is the
/// most expensive thing in this audio path and a Pi 4 cannot afford to pay it
/// twice — which is affordable precisely because stems do not get their own
/// effects. It also means the stems cannot drift apart: one read position, one
/// scaler, one set of hints.
///
/// The cost of that choice: a stem toggle takes effect after the scaler's
/// look-ahead, tens of milliseconds, rather than at an exact sample.
///
/// A track without stems reads exactly as it did before — same path, same
/// cost. The second reader is only created the first time a stems track is
/// loaded, so a deck that never sees one never pays for it.
class StemCachingReader : public CachingReader {
    Q_OBJECT
  public:
    StemCachingReader(const QString& group, UserSettingsPointer pConfig);
    ~StemCachingReader() override;

    /// Sums the two stems when they are in use, otherwise the base reader.
    /// Engine callback thread.
    ReadResult read(SINT startSample, SINT numSamples, bool reverse, CSAMPLE* buffer) override;

    void process() override;
    /// Reports the track the DJ loaded, not the stem file the audio is coming
    /// from: the deck's beatgrid, cues, key and waveform belong to the track.
    void onWorkerTrackLoaded(TrackPointer pTrack,
            mixxx::audio::SampleRate trackSampleRate,
            double trackNumSamples) override;
    void hintAndMaybeWake(const HintVector& hintList) override;
    /// Resolves the track's stems (GUI thread) and points both readers at
    /// them, or at the track itself when it has none.
    void newTrack(TrackPointer pTrack) override;
    void setScheduler(EngineWorkerScheduler* pScheduler) override;

    /// Scale both stems by their gains — ramping `pGains` towards `pTargets`
    /// so a toggle is not heard as a click — and sum the instrumental into the
    /// vocal buffer, which becomes the deck's audio. `pGains` is updated.
    /// Public so the mixing can be tested without an audio device.
    static void mixStems(CSAMPLE* pVocals,
            CSAMPLE* pInstrumental,
            SINT numSamples,
            CSAMPLE* pGains,
            const CSAMPLE* pTargets);

  private:
    /// Gain each stem is heading for, from its `[ChannelN],stem_*_enabled`
    /// control. Read on the engine thread, so this reads the control's atomic
    /// value rather than touching Qt.
    double targetGain(int stemIndex) const;

    const QString m_group;
    UserSettingsPointer m_pConfig;

    /// The instrumental half. Null until the deck first loads a stems track.
    std::unique_ptr<CachingReader> m_pInstrumental;
    /// What the base reader is holding: the vocal stem, or the plain track.
    bool m_stemsActive;
    /// The track the deck loaded, while the stems supply its audio.
    TrackPointer m_pDeckTrack;

    /// `[ChannelN],stem_vocals_enabled` / `stem_instrumental_enabled`, on by
    /// default: a track plays whole until the DJ takes something out of it.
    /// Owned here rather than by the deck so they exist before the skin, the
    /// controller mappings or this reader look for them.
    std::unique_ptr<ControlPushButton> m_pStemEnabled[2];
    /// `[ChannelN],stem_available`: set when a loaded track has stems.
    std::unique_ptr<ControlObject> m_pStemAvailable;
    /// Current gains, ramped towards the targets across a buffer so a toggle
    /// does not land as a click.
    CSAMPLE m_gain[2];
    /// Scratch for the second stem; grown to the largest buffer asked for.
    std::vector<CSAMPLE> m_mixBuffer;
    /// Kept so a reader created mid-session joins the same worker pool.
    EngineWorkerScheduler* m_pScheduler;
};
