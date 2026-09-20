#pragma once

#include <QString>

/// Bite DJ: the pre-separated stems that sit beside a track on its drive.
///
/// `RPI/scripts/prepare-library.sh` prepares them on a workstation,
/// as a directory named after the track:
///
///     Music/Artist - Title.mp3
///     Music/Artist - Title.mp3.stems/
///         vocals.opus
///         instrumental.opus
///         manifest.json
///
/// The two stems sum to the track, so playing both is playing the track; the
/// DJ drops one and the other keeps going. See RPI/bitedj_docs/stems.md.
class StemSet {
  public:
    /// Look for stems beside the track at `trackLocation` and check that they
    /// belong to *this* track. Returns an invalid set when there are none, or
    /// when what is there does not match — a track re-encoded or replaced
    /// under the same name leaves its old stems behind, and playing those
    /// would be playing a different song.
    ///
    /// Cheap enough for the load path: a stat of the track, a small JSON read,
    /// and a stat of each stem. Nothing decodes here.
    static StemSet forTrackLocation(const QString& trackLocation);

    bool isValid() const {
        return !m_vocalsPath.isEmpty() && !m_instrumentalPath.isEmpty();
    }
    const QString& vocalsPath() const {
        return m_vocalsPath;
    }
    const QString& instrumentalPath() const {
        return m_instrumentalPath;
    }
    /// Why an existing stems directory was rejected, for the log. Empty when
    /// the track simply has none, which is the ordinary case.
    const QString& rejection() const {
        return m_rejection;
    }

  private:
    QString m_vocalsPath;
    QString m_instrumentalPath;
    QString m_rejection;
};
