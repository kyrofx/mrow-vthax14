#include "harness/harnessbridge.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlQuery>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_harnessbridge.cpp"
#include "notifications/notifications.h"
#include "preferences/systemsettings.h"
#include "track/track.h"
#include "util/logger.h"
#include "util/usbdevice.h"

namespace {

const mixxx::Logger kLogger("HarnessBridge");
const QString kMountScopePrefix = QStringLiteral("mount:");

/// The drive a sync scope names; a "mount:" scope has no UUID.
mixxx::harness::Drive driveForScope(const QString& mountPoint, const QString& scope) {
    return {mountPoint, scope.startsWith(kMountScopePrefix) ? QString() : scope};
}

const QString kGroup = QStringLiteral("[Harness]");
const ConfigKey kConfigEnabled(kGroup, QStringLiteral("enabled"));
const ConfigKey kConfigUrl(kGroup, QStringLiteral("url"));
const ConfigKey kConfigSession(kGroup, QStringLiteral("session"));
const QString kDefaultUrl = QStringLiteral("http://127.0.0.1:8765");

/// Same window SetlogFeature applies before counting a replay as a new play
/// (kHistoryTrackDuplicateDistanceDefault), so the harness and the drive's
/// History agree on what was played.
constexpr int kReplayWindow = 6;

/// Plays, ratings and syncs are small; anything this slow is a hung harness.
constexpr int kRequestTimeoutMillis = 5000;
/// Suggestions may wait on the cloud model, which the harness itself times out
/// (MROW_MODEL_TIMEOUT, 10 s by default) before falling back to its own
/// ranking. Leave it room to do that and still answer.
constexpr int kSuggestionTimeoutMillis = 20000;
constexpr int kFirstRetryMillis = 1000;
constexpr int kMaxRetryMillis = 15000;
constexpr int kSyncIntervalMillis = 15000;
/// Requests kept while the harness is unreachable. A set is a few dozen plays
/// and ratings; beyond this something is badly wrong and the oldest go.
constexpr int kMaxQueuedRequests = 500;

QString ratingName(HarnessBridge::Rating rating) {
    switch (rating) {
    case HarnessBridge::Rating::Good:
        return QStringLiteral("good");
    case HarnessBridge::Rating::Mid:
        return QStringLiteral("mid");
    case HarnessBridge::Rating::Bad:
        return QStringLiteral("bad");
    case HarnessBridge::Rating::None:
        break;
    }
    return QString();
}

QString trackLabel(const QString& artist, const QString& title) {
    if (artist.isEmpty()) {
        return title;
    }
    return artist + QStringLiteral(" - ") + title;
}

/// Connection-level failures worth retrying; anything else (a 4xx) means the
/// request itself was refused and would be refused again.
bool isTransient(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownServerError:
        return true;
    default:
        return false;
    }
}

QJsonObject readJson(QNetworkReply* pReply) {
    return QJsonDocument::fromJson(pReply->readAll()).object();
}

} // anonymous namespace

QAtomicPointer<HarnessBridge> HarnessBridge::s_pInstance = nullptr;

// Deliberately unparented: CoreServices owns this through a unique_ptr and drops
// it before the PlayerManager it loads into.
HarnessBridge::HarnessBridge(UserSettingsPointer pConfig,
        std::shared_ptr<PlayerManager> pPlayerManager,
        std::shared_ptr<TrackCollectionManager> pTrackCollectionManager)
        : m_pConfig(pConfig),
          m_pPlayerManager(std::move(pPlayerManager)),
          m_pTrackCollectionManager(std::move(pTrackCollectionManager)),
          m_enabled(m_pConfig->getValue(kConfigEnabled, true)),
          m_baseUrl(m_pConfig->getValue(kConfigUrl, kDefaultUrl)),
          m_status(Status::Offline),
          m_requestInFlight(false),
          m_retryDelayMillis(kFirstRetryMillis),
          m_suggestionsInFlight(false),
          m_suggestionsDirty(false),
          m_pCoStatus(nullptr),
          m_pCoCurrentRating(nullptr),
          m_pCoSuggestionCount(nullptr) {
    s_pInstance.storeRelease(this);
    setupControls();

    // A restart mid-set (a crash, a pulled plug) resumes tonight's set rather
    // than starting another one.
    const QString stored = m_pConfig->getValue(kConfigSession, QString());
    const QDate today = QDate::currentDate();
    m_session = mixxx::harness::isSessionFrom(stored, today)
            ? stored
            : mixxx::harness::nextSessionName(today, QString());
    m_pConfig->setValue(kConfigSession, m_session);

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &HarnessBridge::probeHealth);

    if (!m_enabled) {
        m_statusDetail = tr("Assist is turned off");
        kLogger.info() << "Disabled by" << kConfigEnabled;
        return;
    }
    m_statusDetail = tr("Connecting to the assistant");
    kLogger.info() << "Harness at" << m_baseUrl.toString() << "session" << m_session;

    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &HarnessBridge::onCurrentPlayingTrackChanged);

    if (SystemSettings* pSettings = SystemSettings::tryInstance()) {
        connect(pSettings,
                &SystemSettings::usbRowsChanged,
                this,
                [this](const QStringList&) { syncDrives(); });
        connect(pSettings,
                &SystemSettings::mountEjected,
                this,
                &HarnessBridge::onMountEjected);
    }
    // The Rekordbox catalog of a drive is parsed in the background some time
    // after it mounts; poll for it rather than hook the parser.
    m_syncTimer.setInterval(kSyncIntervalMillis);
    connect(&m_syncTimer, &QTimer::timeout, this, &HarnessBridge::syncDrives);
    m_syncTimer.start();

    probeHealth();
}

HarnessBridge::~HarnessBridge() {
    s_pInstance.storeRelease(nullptr);
}

void HarnessBridge::setupControls() {
    auto addButton = [this](const QString& item, std::function<void()> action) {
        auto pButton = std::make_unique<ControlPushButton>(ConfigKey(kGroup, item));
        connect(pButton.get(),
                &ControlObject::valueChanged,
                this,
                [action = std::move(action)](double value) {
                    if (value > 0) {
                        action();
                    }
                });
        m_controls.push_back(std::move(pButton));
    };
    auto addIndicator = [this](const QString& item) {
        auto pControl = std::make_unique<ControlObject>(ConfigKey(kGroup, item));
        pControl->setReadOnly();
        ControlObject* pRaw = pControl.get();
        m_controls.push_back(std::move(pControl));
        return pRaw;
    };

    addButton(QStringLiteral("rate_good"), [this] { rate(Rating::Good); });
    addButton(QStringLiteral("rate_mid"), [this] { rate(Rating::Mid); });
    addButton(QStringLiteral("rate_bad"), [this] { rate(Rating::Bad); });
    addButton(QStringLiteral("refresh"), [this] { refreshSuggestions(); });
    for (int i = 1; i <= kSuggestionCount; ++i) {
        addButton(QStringLiteral("skip_suggestion_%1").arg(i),
                [this, i] { skipSuggestion(i - 1); });
        for (int deck = 1; deck <= kLoadDeckCount; ++deck) {
            addButton(QStringLiteral("load_suggestion_%1_deck_%2").arg(i).arg(deck),
                    [this, i, deck] { loadSuggestion(i - 1, deck); });
        }
    }
    m_pCoStatus = addIndicator(QStringLiteral("status"));
    m_pCoCurrentRating = addIndicator(QStringLiteral("current_rating"));
    m_pCoSuggestionCount = addIndicator(QStringLiteral("suggestion_count"));

    // The same control that clears the "played tonight" tint (Library) starts
    // a new set here: it is how the next DJ takes over.
    m_pResetPlayedTracks = std::make_unique<ControlProxy>(
            QStringLiteral("[Library]"),
            QStringLiteral("reset_played_tracks"),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pResetPlayedTracks->connectValueChanged(this, [this](double value) {
        if (value > 0 && m_enabled) {
            startNewSession();
        }
    });
}

QString HarnessBridge::currentTrackLabel() const {
    return m_current.label;
}

void HarnessBridge::onCurrentPlayingTrackChanged(TrackPointer pTrack) {
    if (!pTrack) {
        return;
    }
    const QString location = pTrack->getLocation();
    const int recentIndex = m_recentLocations.indexOf(location);
    if (recentIndex >= 0) {
        // Back on a track from the last few: the same play resumed, not a new
        // one. Move it to the front, as SetlogFeature does.
        m_recentLocations.move(recentIndex, 0);
        return;
    }
    m_recentLocations.prepend(location);
    while (m_recentLocations.size() > kReplayWindow) {
        m_recentLocations.removeLast();
    }

    const QList<mixxx::harness::Drive> drives = mountedDrives();
    const mixxx::harness::Drive* pDrive = mixxx::harness::driveForLocation(location, drives);
    Play play;
    play.trackId = mixxx::harness::trackIdForLocation(location, drives);
    play.scope = pDrive ? mixxx::harness::scopeForDrive(*pDrive) : QStringLiteral("local");
    play.title = pTrack->getTitle();
    if (play.title.isEmpty()) {
        play.title = QFileInfo(location).completeBaseName();
    }
    play.artist = pTrack->getArtist();
    play.genre = pTrack->getGenre();
    play.key = mixxx::harness::camelotForKey(pTrack->getKey());
    play.location = location;
    play.durationSeconds = pTrack->getDuration();
    const int deckIndex = PlayerInfo::instance().getCurrentPlayingDeck();
    if (deckIndex >= 0) {
        play.group = PlayerManager::groupForDeck(deckIndex);
        // What the crowd heard: the deck's rate-adjusted tempo, not the file's.
        play.bpm = ControlObject::get(ConfigKey(play.group, QStringLiteral("bpm")));
    }
    if (play.bpm <= 0) {
        play.bpm = pTrack->getBpm();
    }
    reportPlay(play);
}

void HarnessBridge::reportPlay(const Play& play) {
    if (!m_enabled || play.trackId.isEmpty()) {
        return;
    }
    // Stable across retries of this play, unique across plays: a retry after a
    // timeout the harness did see returns the same play instead of a second one.
    const QString eventId = m_session + QLatin1Char(':') + play.group + QLatin1Char(':') +
            QString::number(QDateTime::currentMSecsSinceEpoch());
    m_current = CurrentPlay();
    m_current.trackId = play.trackId;
    m_current.label = trackLabel(play.artist, play.title);
    m_current.eventId = eventId;
    setCurrentRating(Rating::None);

    QJsonObject track{
            {QStringLiteral("id"), play.trackId},
            {QStringLiteral("title"), play.title},
            {QStringLiteral("artist"), play.artist},
            {QStringLiteral("genre"), play.genre},
            {QStringLiteral("path"), play.location},
    };
    if (play.bpm > 0) {
        track.insert(QStringLiteral("bpm"), play.bpm);
    }
    if (!play.key.isEmpty()) {
        track.insert(QStringLiteral("camelot"), play.key);
    }
    if (play.durationSeconds > 0) {
        track.insert(QStringLiteral("duration"), play.durationSeconds);
    }
    QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("track_id"), play.trackId},
            {QStringLiteral("event_id"), eventId},
            {QStringLiteral("scope"), play.scope},
            {QStringLiteral("track"), track},
    };
    if (play.bpm > 0) {
        body.insert(QStringLiteral("bpm"), play.bpm);
    }
    enqueue({QStringLiteral("/api/play"), body, [this, eventId](const QJsonObject& reply) {
                 if (m_current.eventId != eventId) {
                     return; // A later play has already replaced this one.
                 }
                 m_current.playId = reply.value(QStringLiteral("play_id")).toInteger();
                 if (m_current.ratingPending) {
                     sendRating();
                 }
                 requestSuggestions();
             }});
    emit stateChanged();
}

void HarnessBridge::rate(Rating rating) {
    if (!m_enabled || rating == Rating::None) {
        return;
    }
    if (m_current.trackId.isEmpty()) {
        publish(tr("Nothing has played yet to rate"), true);
        return;
    }
    setCurrentRating(rating);
    if (m_current.playId > 0) {
        sendRating();
    } else {
        // The play is still on its way to the harness; the rating follows it.
        m_current.ratingPending = true;
    }
    emit stateChanged();
}

void HarnessBridge::sendRating() {
    m_current.ratingPending = false;
    const QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("track_id"), m_current.trackId},
            {QStringLiteral("play_id"), m_current.playId},
            {QStringLiteral("rating"), ratingName(m_current.rating)},
    };
    enqueue({QStringLiteral("/api/feedback"), body, [this](const QJsonObject&) {
                 requestSuggestions();
             }});
}

void HarnessBridge::setCurrentRating(Rating rating) {
    m_current.rating = rating;
    if (m_pCoCurrentRating) {
        m_pCoCurrentRating->forceSet(static_cast<double>(rating));
    }
}

void HarnessBridge::skipSuggestion(int index) {
    if (!m_enabled || index < 0 || index >= m_suggestions.size()) {
        return;
    }
    const Suggestion skipped = m_suggestions.takeAt(index);
    m_pCoSuggestionCount->forceSet(m_suggestions.size());
    emit stateChanged();
    const QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("track_id"), skipped.trackId},
            {QStringLiteral("rating"), QStringLiteral("skip")},
    };
    enqueue({QStringLiteral("/api/feedback"), body, [this](const QJsonObject&) {
                 requestSuggestions();
             }});
}

void HarnessBridge::loadSuggestion(int index, int deckNumber) {
    if (!m_enabled || index < 0 || index >= m_suggestions.size() || !m_pPlayerManager) {
        return;
    }
    if (deckNumber < 1 || deckNumber > m_pPlayerManager->numberOfDecks()) {
        return;
    }
    const QString group = PlayerManager::groupForDeck(deckNumber - 1);
    if (ControlObject::toBool(ConfigKey(group, QStringLiteral("play")))) {
        // Suggest, never interrupt: the playing deck is the set.
        publish(tr("Deck %1 is playing; load into the other deck").arg(deckNumber), true);
        return;
    }
    const Suggestion& suggestion = m_suggestions.at(index);
    QString location = mixxx::harness::locationForTrackId(suggestion.trackId, mountedDrives());
    if (location.isEmpty()) {
        location = suggestion.path;
    }
    if (location.isEmpty() || !QFileInfo::exists(location)) {
        publish(tr("\"%1\" is on a drive that is not plugged in").arg(suggestion.title), true);
        return;
    }
    m_pPlayerManager->slotLoadLocationToPlayer(location, group, false);
}

void HarnessBridge::refreshSuggestions() {
    if (!m_enabled) {
        return;
    }
    if (m_status == Status::Offline) {
        probeHealth();
        return;
    }
    requestSuggestions();
}

void HarnessBridge::startNewSession() {
    m_session = mixxx::harness::nextSessionName(QDate::currentDate(), m_session);
    m_pConfig->setValue(kConfigSession, m_session);
    m_current = CurrentPlay();
    setCurrentRating(Rating::None);
    m_recentLocations.clear();
    kLogger.info() << "New set" << m_session;
    emit stateChanged();
    requestSuggestions();
}

// ---- Transport -------------------------------------------------------------

QNetworkReply* HarnessBridge::post(
        const QString& path, const QJsonObject& body, int timeoutMillis) {
    QUrl url = m_baseUrl;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(timeoutMillis);
    return m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void HarnessBridge::enqueue(Request request) {
    if (m_queue.size() >= kMaxQueuedRequests) {
        kLogger.warning() << "Queue full; dropping" << m_queue.first().path;
        m_queue.removeFirst();
    }
    m_queue.append(std::move(request));
    sendNext();
}

void HarnessBridge::sendNext() {
    if (m_requestInFlight || m_queue.isEmpty() || m_retryTimer.isActive()) {
        return;
    }
    m_requestInFlight = true;
    const Request& request = m_queue.first();
    QNetworkReply* pReply = post(request.path, request.body, kRequestTimeoutMillis);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply] {
        pReply->deleteLater();
        m_requestInFlight = false;
        if (m_queue.isEmpty()) {
            return;
        }
        const QNetworkReply::NetworkError error = pReply->error();
        if (error != QNetworkReply::NoError && isTransient(error)) {
            // Keep it at the head: order matters (a rating needs its play).
            scheduleRetry(pReply->errorString());
            return;
        }
        Request done = m_queue.takeFirst();
        const QJsonObject reply = readJson(pReply);
        if (error != QNetworkReply::NoError) {
            // Refused as invalid: retrying would be refused again.
            kLogger.warning() << done.path << "rejected:"
                              << reply.value(QStringLiteral("error")).toString(
                                         pReply->errorString());
        } else {
            m_retryDelayMillis = kFirstRetryMillis;
            if (m_status == Status::Offline) {
                setStatus(Status::Heuristic, QString());
            }
            if (done.onSuccess) {
                done.onSuccess(reply);
            }
        }
        sendNext();
    });
}

void HarnessBridge::scheduleRetry(const QString& reason) {
    if (m_status != Status::Offline) {
        kLogger.warning() << "Harness unreachable:" << reason;
    }
    setStatus(Status::Offline, tr("Assistant not running"));
    m_retryTimer.start(m_retryDelayMillis);
    m_retryDelayMillis = std::min(m_retryDelayMillis * 2, kMaxRetryMillis);
}

void HarnessBridge::probeHealth() {
    QUrl url = m_baseUrl;
    url.setPath(QStringLiteral("/health"));
    QNetworkRequest request(url);
    request.setTransferTimeout(kRequestTimeoutMillis);
    QNetworkReply* pReply = m_network.get(request);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply] {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            scheduleRetry(pReply->errorString());
            return;
        }
        const bool wasOffline = m_status == Status::Offline;
        m_retryDelayMillis = kFirstRetryMillis;
        if (wasOffline) {
            kLogger.info() << "Harness reachable";
            setStatus(Status::Heuristic, QString());
            syncDrives();
        }
        sendNext();
        requestSuggestions();
    });
}

void HarnessBridge::requestSuggestions() {
    if (!m_enabled) {
        return;
    }
    if (m_suggestionsInFlight) {
        // Something changed while the last request was out; ask again after.
        m_suggestionsDirty = true;
        return;
    }
    m_suggestionsInFlight = true;
    m_suggestionsDirty = false;
    const QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("count"), kSuggestionCount},
    };
    QNetworkReply* pReply = post(QStringLiteral("/api/recommend"), body, kSuggestionTimeoutMillis);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply] {
        pReply->deleteLater();
        m_suggestionsInFlight = false;
        if (pReply->error() != QNetworkReply::NoError) {
            if (isTransient(pReply->error())) {
                scheduleRetry(pReply->errorString());
            } else {
                kLogger.warning() << "Suggestions rejected:" << pReply->errorString();
            }
            return;
        }
        const QJsonObject reply = readJson(pReply);
        m_suggestions.clear();
        const QJsonArray tracks = reply.value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& value : tracks) {
            const QJsonObject track = value.toObject();
            Suggestion suggestion;
            suggestion.trackId = track.value(QStringLiteral("id")).toString();
            suggestion.title = track.value(QStringLiteral("title")).toString();
            suggestion.artist = track.value(QStringLiteral("artist")).toString();
            suggestion.key = track.value(QStringLiteral("camelot")).toString();
            suggestion.bpm = track.value(QStringLiteral("bpm")).toDouble();
            suggestion.path = track.value(QStringLiteral("path")).toString();
            suggestion.reason = track.value(QStringLiteral("model_reason")).toString();
            if (suggestion.reason.isEmpty()) {
                suggestion.reason = track.value(QStringLiteral("reasons"))
                                            .toArray()
                                            .first()
                                            .toString();
            }
            m_suggestions.append(suggestion);
        }
        m_pCoSuggestionCount->forceSet(m_suggestions.size());
        const QString modelError = reply.value(QStringLiteral("model_error")).toString();
        if (reply.value(QStringLiteral("source")).toString() == QStringLiteral("model")) {
            setStatus(Status::Model, QString());
        } else {
            setStatus(Status::Heuristic, modelError);
        }
        emit stateChanged();
        if (m_suggestionsDirty) {
            requestSuggestions();
        }
    });
}

void HarnessBridge::setStatus(Status status, const QString& detail) {
    if (m_status == status && m_statusDetail == detail) {
        return;
    }
    m_status = status;
    m_statusDetail = detail;
    if (m_pCoStatus) {
        m_pCoStatus->forceSet(static_cast<double>(status));
    }
    emit stateChanged();
}

// ---- Library sync ----------------------------------------------------------

QList<mixxx::harness::Drive> HarnessBridge::mountedDrives() const {
    QList<mixxx::harness::Drive> drives;
    const QStringList mountPoints = SystemSettings::usbMountPoints();
    for (const QString& mountPoint : mountPoints) {
        const QString cleaned = QDir::cleanPath(mountPoint);
        // Known drives come from the cache; a drive the sync has not seen yet
        // (plugged in within the last poll) is asked for its UUID directly.
        QString scope = m_scopeByMount.value(cleaned);
        if (scope.isEmpty()) {
            scope = mixxx::harness::scopeForDrive({cleaned, mixxx::volumeUuidForMountPoint(cleaned)});
        }
        drives.append(driveForScope(cleaned, scope));
    }
    return drives;
}

int HarnessBridge::catalogSizeForDrive(const mixxx::harness::Drive& drive) const {
    if (!m_pTrackCollectionManager) {
        return 0;
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM rekordbox_library "
            "WHERE substr(location, 1, :length) = :prefix"));
    const QString prefix = drive.mountPoint + QLatin1Char('/');
    query.bindValue(QStringLiteral(":length"), prefix.size());
    query.bindValue(QStringLiteral(":prefix"), prefix);
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QJsonArray HarnessBridge::catalogForDrive(const mixxx::harness::Drive& drive) const {
    QJsonArray tracks;
    if (!m_pTrackCollectionManager) {
        return tracks;
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    // rekordbox_library is the fork's mirror of every Rekordbox export on a
    // mounted drive (RekordboxFeature); it may not exist yet.
    query.prepare(QStringLiteral(
            "SELECT location, title, artist, genre, duration, bpm, key "
            "FROM rekordbox_library WHERE substr(location, 1, :length) = :prefix"));
    const QString prefix = drive.mountPoint + QLatin1Char('/');
    query.bindValue(QStringLiteral(":length"), prefix.size());
    query.bindValue(QStringLiteral(":prefix"), prefix);
    if (!query.exec()) {
        return tracks;
    }
    const QList<mixxx::harness::Drive> drives{drive};
    while (query.next()) {
        const QString location = query.value(0).toString();
        QJsonObject track{
                {QStringLiteral("id"), mixxx::harness::trackIdForLocation(location, drives)},
                {QStringLiteral("title"), query.value(1).toString()},
                {QStringLiteral("artist"), query.value(2).toString()},
                {QStringLiteral("genre"), query.value(3).toString()},
                {QStringLiteral("path"), location},
        };
        if (track.value(QStringLiteral("title")).toString().isEmpty()) {
            track.insert(QStringLiteral("title"), QFileInfo(location).completeBaseName());
        }
        const int duration = query.value(4).toInt();
        if (duration > 0) {
            track.insert(QStringLiteral("duration"), duration);
        }
        const double bpm = query.value(5).toDouble();
        if (bpm > 0) {
            track.insert(QStringLiteral("bpm"), bpm);
        }
        const QString camelot = mixxx::harness::camelotForKeyText(query.value(6).toString());
        if (!camelot.isEmpty()) {
            track.insert(QStringLiteral("camelot"), camelot);
        }
        tracks.append(track);
    }
    return tracks;
}

void HarnessBridge::syncDrives() {
    if (!m_enabled) {
        return;
    }
    QHash<QString, QString> scopeByMount;
    for (const QString& mountPoint : SystemSettings::usbMountPoints()) {
        const QString cleaned = QDir::cleanPath(mountPoint);
        mixxx::harness::Drive drive{cleaned, mixxx::volumeUuidForMountPoint(cleaned)};
        scopeByMount.insert(cleaned, mixxx::harness::scopeForDrive(drive));
    }
    // Drives that went away without an eject event (a yank the poll caught).
    const QStringList knownMounts = m_scopeByMount.keys();
    for (const QString& mountPoint : knownMounts) {
        if (!scopeByMount.contains(mountPoint)) {
            onMountEjected(mountPoint);
        }
    }
    m_scopeByMount = scopeByMount;

    for (auto it = scopeByMount.cbegin(); it != scopeByMount.cend(); ++it) {
        const QString scope = it.value();
        const mixxx::harness::Drive drive = driveForScope(it.key(), scope);
        // Cheap check first: this runs every poll, and a drive's catalog only
        // changes when Rekordbox finishes (re)parsing it.
        const int size = catalogSizeForDrive(drive);
        if (size == 0 || m_syncedCountByScope.value(scope, -1) == size) {
            continue;
        }
        const QJsonArray catalog = catalogForDrive(drive);
        const int count = catalog.size();
        const QJsonObject body{
                {QStringLiteral("scope"), scope},
                {QStringLiteral("complete"), true},
                {QStringLiteral("tracks"), catalog},
        };
        m_syncedCountByScope.insert(scope, count);
        enqueue({QStringLiteral("/api/library/sync"), body, [this, scope, count](const QJsonObject& reply) {
                     kLogger.info() << "Synced" << reply.value(QStringLiteral("synced")).toInt()
                                    << "of" << count << "tracks from drive" << scope << "("
                                    << reply.value(QStringLiteral("skipped_count")).toInt()
                                    << "not analyzed yet)";
                     requestSuggestions();
                 }});
    }
}

void HarnessBridge::onMountEjected(const QString& mountPoint) {
    const QString cleaned = QDir::cleanPath(mountPoint);
    const QString scope = m_scopeByMount.take(cleaned);
    if (scope.isEmpty() || !m_enabled) {
        return;
    }
    m_syncedCountByScope.remove(scope);
    enqueue({QStringLiteral("/api/library/unavailable"),
            QJsonObject{{QStringLiteral("scope"), scope}},
            [this](const QJsonObject&) { requestSuggestions(); }});
}

void HarnessBridge::publish(const QString& message, bool warning) const {
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->publish(message,
                warning ? Notifications::Severity::Warning : Notifications::Severity::Info);
    }
}
