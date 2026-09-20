#pragma once

#include <QString>
#include <memory>

class ControlObject;
class ControlPushButton;

/// Bite DJ: the deck controls for pre-separated stems — the contract the skin's
/// S panel, controller mappings and (later) the engine all bind to.
///
/// A separated track carries two stems beside it on the drive, vocals and
/// instrumental (see RPI/bitedj_docs/stems.md). The DJ drops either one while
/// the other keeps playing; effects and EQ stay on the deck as a whole, which
/// is what lets the stems be mixed before the timestretcher instead of after —
/// one scaler per deck rather than one per stem.
///
/// **Only the controls exist so far.** Nothing reads them in the audio path
/// yet: `stem_available` is always 0, and toggling a stem changes nothing you
/// can hear. This is deliberate — it lets the panel be built and looked at
/// before the engine work lands, and it is the seam that work plugs into.
class StemControls {
  public:
    /// Stems a separated track carries, in the order the panel shows them.
    static constexpr int kStemCount = 2;

    explicit StemControls(const QString& group);
    ~StemControls();

    /// Whether the loaded track has stems beside it on its drive. Read-only to
    /// the skin: it follows the track, not the DJ.
    void setAvailable(bool available);
    bool isEnabled(int stemIndex) const;

  private:
    /// Read-only indicator; the skin greys the S button out when this is 0.
    std::unique_ptr<ControlObject> m_pAvailable;
    /// One toggle per stem, on by default: a track plays whole until the DJ
    /// takes something out of it.
    std::unique_ptr<ControlPushButton> m_pStemEnabled[kStemCount];
};
