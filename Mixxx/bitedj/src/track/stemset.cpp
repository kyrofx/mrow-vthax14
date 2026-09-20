#include "track/stemset.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Must match RPI/scripts/separate-stems.py.
constexpr int kManifestVersion = 1;
const QString kStemsSuffix = QStringLiteral(".stems");
const QString kManifestName = QStringLiteral("manifest.json");
const QString kVocals = QStringLiteral("vocals.opus");
const QString kInstrumental = QStringLiteral("instrumental.opus");

// A manifest is a few hundred bytes. Anything larger is not one of ours.
constexpr qint64 kMaxManifestBytes = 64 * 1024;

} // anonymous namespace

StemSet StemSet::forTrackLocation(const QString& trackLocation) {
    StemSet result;
    const QFileInfo trackInfo(trackLocation);
    const QString directory = trackLocation + kStemsSuffix;
    QFile manifestFile(directory + QLatin1Char('/') + kManifestName);
    if (!manifestFile.exists()) {
        return result; // No stems for this track; the ordinary case.
    }
    if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > kMaxManifestBytes) {
        result.m_rejection = QStringLiteral("manifest unreadable");
        return result;
    }
    const QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.read(kMaxManifestBytes)).object();
    if (manifest.value(QStringLiteral("version")).toInt() != kManifestVersion) {
        // Written by a newer separation pass than this build understands.
        result.m_rejection = QStringLiteral("manifest version %1")
                                     .arg(manifest.value(QStringLiteral("version")).toInt());
        return result;
    }

    // The source's size is what ties these stems to this file. A re-encode
    // keeps the name and changes the bytes, which is exactly the case that
    // would otherwise play the wrong song's vocals over this one.
    const QJsonObject source = manifest.value(QStringLiteral("source")).toObject();
    if (source.value(QStringLiteral("name")).toString() != trackInfo.fileName() ||
            source.value(QStringLiteral("bytes")).toVariant().toLongLong() !=
                    trackInfo.size()) {
        result.m_rejection = QStringLiteral("stems belong to a different file");
        return result;
    }

    const QJsonObject stems = manifest.value(QStringLiteral("stems")).toObject();
    const QString vocals = directory + QLatin1Char('/') +
            stems.value(QStringLiteral("vocals")).toString(kVocals);
    const QString instrumental = directory + QLatin1Char('/') +
            stems.value(QStringLiteral("instrumental")).toString(kInstrumental);
    if (!QFileInfo::exists(vocals) || !QFileInfo::exists(instrumental)) {
        result.m_rejection = QStringLiteral("a stem file is missing");
        return result;
    }

    result.m_vocalsPath = vocals;
    result.m_instrumentalPath = instrumental;
    return result;
}
