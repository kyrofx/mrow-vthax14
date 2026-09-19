#pragma once

#include <QDate>
#include <QList>
#include <QString>

#include "proto/keys.pb.h"

/// Pure helpers shared by HarnessBridge and its tests: how a track, a drive
/// and a set are named when talking to the harness sidecar.
namespace mixxx::harness {

/// A mounted removable drive, as the harness knows it.
struct Drive {
    /// Cleaned absolute mount point.
    QString mountPoint;
    /// Filesystem UUID; empty when the volume has none.
    QString uuid;
};

/// The harness's name for a drive (its sync scope): the filesystem UUID, which
/// survives a re-plug under another mount name, or "mount:<mount point>" for a
/// volume without one.
QString scopeForDrive(const Drive& drive);

/// The drive `location` lives on, or nullptr for a file on the unit itself.
/// The longest matching mount point wins, so nested mounts resolve correctly.
const Drive* driveForLocation(const QString& location, const QList<Drive>& drives);

/// Stable id for the track at `location`: "<scope>:<path relative to the
/// drive>" for a track on a drive (the same relative key the .bitedj stores
/// use), "local:<absolute path>" otherwise. Unlike a Mixxx TrackId this is the
/// same on every unit and after every re-plug, so history and crowd feedback
/// follow the file.
QString trackIdForLocation(const QString& location, const QList<Drive>& drives);

/// Where the track `trackId` is right now, or an empty string when the drive it
/// lives on is not mounted.
QString locationForTrackId(const QString& trackId, const QList<Drive>& drives);

/// Camelot notation ("8A" for A minor, "8B" for C major) as the harness's
/// scorer expects it; empty for an unknown key.
QString camelotForKey(mixxx::track::io::key::ChromaticKey key);
/// camelotForKey() for key text as a library or a Rekordbox export spells it
/// ("Am", "A minor", "8A", "F#m"...).
QString camelotForKeyText(const QString& text);

/// Name for a set starting on `today`: the ISO date, then "#2", "#3"... when
/// `previous` is already a set from the same day. Same shape as the fork's
/// per-drive history sessions.
QString nextSessionName(const QDate& today, const QString& previous);

/// Whether `session` is a set that started on `today`, i.e. should be resumed
/// after a restart rather than replaced.
bool isSessionFrom(const QString& session, const QDate& today);

} // namespace mixxx::harness
