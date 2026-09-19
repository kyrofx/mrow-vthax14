#include "harness/harnessids.h"

#include <QDir>

#include "track/keyutils.h"

namespace mixxx::harness {

namespace {

const QString kLocalPrefix = QStringLiteral("local:");
const QString kMountScopePrefix = QStringLiteral("mount:");
const QChar kSeparator = QLatin1Char(':');
const QString kSessionSuffix = QStringLiteral("#");

using mixxx::track::io::key::ChromaticKey;

/// Camelot codes indexed by ChromaticKey (C_MAJOR = 1 ... B_MINOR = 24).
const char* const kCamelot[] = {
        "",    // INVALID
        "8B",  // C major
        "3B",  // D flat major
        "10B", // D major
        "5B",  // E flat major
        "12B", // E major
        "7B",  // F major
        "2B",  // F sharp major
        "9B",  // G major
        "4B",  // A flat major
        "11B", // A major
        "6B",  // B flat major
        "1B",  // B major
        "5A",  // C minor
        "12A", // C sharp minor
        "7A",  // D minor
        "2A",  // E flat minor
        "9A",  // E minor
        "4A",  // F minor
        "11A", // F sharp minor
        "6A",  // G minor
        "1A",  // G sharp minor
        "8A",  // A minor
        "3A",  // B flat minor
        "10A", // B minor
};

bool isUnder(const QString& location, const QString& mountPoint) {
    if (mountPoint.isEmpty()) {
        return false;
    }
    if (mountPoint == QStringLiteral("/")) {
        return location.startsWith(mountPoint);
    }
    return location.startsWith(mountPoint + QLatin1Char('/'));
}

} // anonymous namespace

QString scopeForDrive(const Drive& drive) {
    if (!drive.uuid.isEmpty()) {
        return drive.uuid;
    }
    return kMountScopePrefix + drive.mountPoint;
}

const Drive* driveForLocation(const QString& location, const QList<Drive>& drives) {
    const QString cleaned = QDir::cleanPath(location);
    const Drive* pBest = nullptr;
    for (const Drive& drive : drives) {
        if (isUnder(cleaned, drive.mountPoint) &&
                (!pBest || drive.mountPoint.size() > pBest->mountPoint.size())) {
            pBest = &drive;
        }
    }
    return pBest;
}

QString trackIdForLocation(const QString& location, const QList<Drive>& drives) {
    const QString cleaned = QDir::cleanPath(location);
    const Drive* pDrive = driveForLocation(cleaned, drives);
    if (!pDrive) {
        return kLocalPrefix + cleaned;
    }
    const QString relative = QDir(pDrive->mountPoint).relativeFilePath(cleaned);
    return scopeForDrive(*pDrive) + kSeparator + relative;
}

QString locationForTrackId(const QString& trackId, const QList<Drive>& drives) {
    if (trackId.startsWith(kLocalPrefix)) {
        return trackId.mid(kLocalPrefix.size());
    }
    // Match against the drives rather than parse the id: a "mount:" scope
    // contains a separator of its own, and a relative path may too.
    for (const Drive& drive : drives) {
        const QString prefix = scopeForDrive(drive) + kSeparator;
        if (trackId.startsWith(prefix)) {
            return QDir(drive.mountPoint).filePath(trackId.mid(prefix.size()));
        }
    }
    return QString();
}

QString camelotForKey(ChromaticKey key) {
    const int index = static_cast<int>(key);
    if (index <= 0 || index >= static_cast<int>(std::size(kCamelot))) {
        return QString();
    }
    return QString::fromLatin1(kCamelot[index]);
}

QString camelotForKeyText(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    return camelotForKey(KeyUtils::guessKeyFromText(text));
}

QString nextSessionName(const QDate& today, const QString& previous) {
    const QString date = today.toString(Qt::ISODate);
    if (!isSessionFrom(previous, today)) {
        return date;
    }
    int number = 1;
    const int suffixAt = previous.indexOf(kSessionSuffix);
    if (suffixAt >= 0) {
        number = previous.mid(suffixAt + 1).toInt();
    }
    return date + kSessionSuffix + QString::number(std::max(number, 1) + 1);
}

bool isSessionFrom(const QString& session, const QDate& today) {
    const QString date = today.toString(Qt::ISODate);
    return session == date || session.startsWith(date + kSessionSuffix);
}

} // namespace mixxx::harness
