#include "harness/harnessbridge.h"

#include <QCryptographicHash>
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
#include "track/trackref.h"
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
/// (25 s for runtime Google Cloud Gemini settings, up to 60 s from the environment)
/// before falling back locally. Leave it room to finish and still answer.
constexpr int kSuggestionTimeoutMillis = 65000;
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
    // HTTP is an explicit developer/test override, never the appliance default.
    if (!m_pConfig->getValue(ConfigKey(kGroup, QStringLiteral("external")), false)) {
        m_worker = std::make_unique<HarnessWorker>(
                QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("harness/harness.sqlite3")));
        connect(m_worker.get(), &HarnessWorker::stopped,
                this, &HarnessBridge::scheduleRetry);
    }
    m_statusDetail = tr("Starting the built-in agent");
    kLogger.info() << "Agent session" << m_session << "built-in" << bool(m_worker);

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
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        syncDrives();
        // The agent caches unchanged context, so polling does not spend tokens.
        requestSuggestions();
    });
    m_syncTimer.start();

    m_musicTimer.setInterval(2000);
    connect(&m_musicTimer, &QTimer::timeout, this, &HarnessBridge::pollGeneratedMusic);
    m_musicTimer.start();
    probeHealth();
}

HarnessBridge::~HarnessBridge() {
    s_pInstance.storeRelease(nullptr);
    m_worker.reset();
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
    play.libraryBpm = pTrack->getBpm();
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
    consumeGenerated(play.location);
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
    const double libraryBpm = play.libraryBpm > 0 ? play.libraryBpm : play.bpm;
    if (libraryBpm > 0) {
        track.insert(QStringLiteral("bpm"), libraryBpm);
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
    for (const auto& generated : m_generatedSuggestions) {
        if (generated.trackId == skipped.trackId) {
            consumeGenerated(skipped.path);
            return;
        }
    }
    const QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("track_id"), skipped.trackId},
            {QStringLiteral("rating"), QStringLiteral("skip")},
    };
    enqueue({QStringLiteral("/api/feedback"), body, [this](const QJsonObject&) {
                 requestSuggestions();
             }});
}

bool HarnessBridge::loadSuggestion(int index, int deckNumber) {
    if (!m_enabled || index < 0 || index >= m_suggestions.size() || !m_pPlayerManager || agentBusy()) {
        return false;
    }
    return loadTrack(m_suggestions.at(index), deckNumber);
}

bool HarnessBridge::loadPlanTrack(int index, int deckNumber) {
    const QJsonArray tracks = m_agentPlan.value(QStringLiteral("tracks")).toArray();
    if (index < 0 || index >= tracks.size() || agentBusy()) {
        return false;
    }
    const QJsonObject track = tracks.at(index).toObject();
    Suggestion suggestion;
    suggestion.trackId = track.value(QStringLiteral("id")).toString();
    suggestion.path = track.value(QStringLiteral("path")).toString();
    suggestion.title = track.value(QStringLiteral("title")).toString();
    return loadTrack(suggestion, deckNumber);
}

bool HarnessBridge::loadTrack(const Suggestion& suggestion, int deckNumber) {
    if (!m_enabled || !m_pPlayerManager) {
        return false;
    }
    if (deckNumber < 1 || deckNumber > m_pPlayerManager->numberOfDecks()) {
        return false;
    }
    const QString group = PlayerManager::groupForDeck(deckNumber - 1);
    if (ControlObject::toBool(ConfigKey(group, QStringLiteral("play")))) {
        // Suggest, never interrupt: the playing deck is the set.
        publish(tr("Deck %1 is playing; load into the other deck").arg(deckNumber), true);
        return false;
    }
    QString location = mixxx::harness::locationForTrackId(suggestion.trackId, mountedDrives());
    if (location.isEmpty()) {
        location = suggestion.path;
    }
    if (location.isEmpty() || !QFileInfo::exists(location)) {
        publish(tr("\"%1\" is on a drive that is not plugged in").arg(suggestion.title), true);
        return false;
    }
    m_pPlayerManager->slotLoadLocationToPlayer(location, group, false);
    return true;
}

void HarnessBridge::planSet(int count, const QString& direction, bool clear) {
    QJsonObject options = m_agentPlan.value(QStringLiteral("options")).toObject();
    options.insert(QStringLiteral("direction"), direction);
    m_agentAction = {{QStringLiteral("action"), clear ? QStringLiteral("clear") : QStringLiteral("generate")},
            {QStringLiteral("count"), count},
            {QStringLiteral("options"), options}};
    requestSuggestions();
}

void HarnessBridge::agentRequest(const QString& path, const QJsonObject& body,
        QObject* context, std::function<void(const QJsonObject&)> callback) {
    if (!m_worker && m_baseUrl.host() != QStringLiteral("127.0.0.1") &&
            m_baseUrl.host() != QStringLiteral("localhost") &&
            m_baseUrl.host() != QStringLiteral("::1")) {
        callback(QJsonObject{{QStringLiteral("error"), tr("Agent settings require a localhost assistant URL")}});
        return;
    }
    QJsonObject request = body;
    request.insert(QStringLiteral("session"), m_session);
    call(path, request, kSuggestionTimeoutMillis, context,
            [callback = std::move(callback)](const QJsonObject& result, bool) {
        callback(result);
    });
}

void HarnessBridge::refreshSuggestions() {
    if (!m_enabled) {
        return;
    }
    if (m_status == Status::Offline) {
        probeHealth();
        return;
    }
    m_agentAction.insert(QStringLiteral("retry"), true);
    requestSuggestions();
}

void HarnessBridge::startNewSession() {
    m_session = mixxx::harness::nextSessionName(QDate::currentDate(), m_session);
    m_pConfig->setValue(kConfigSession, m_session);
    m_current = CurrentPlay();
    m_agentPlan = QJsonObject();
    m_agentAction = QJsonObject();
    m_suggestions.clear();
    m_pCoSuggestionCount->forceSet(0);
    setCurrentRating(Rating::None);
    m_recentLocations.clear();
    kLogger.info() << "New set" << m_session;
    emit stateChanged();
    requestSuggestions();
}

// ---- Transport -------------------------------------------------------------

void HarnessBridge::call(const QString& path, const QJsonObject& body, int timeoutMillis,
        QObject* context, HarnessWorker::Callback callback) {
    if (!m_enabled) {
        callback({{QStringLiteral("error"), tr("Assist is turned off")}}, false);
        return;
    }
    if (m_worker) {
        m_worker->request(path, body, timeoutMillis, context, std::move(callback));
        return;
    }
    QNetworkReply* reply = post(path, body, timeoutMillis);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(callback)] {
        QJsonObject result = readJson(reply);
        if (reply->error() != QNetworkReply::NoError && !result.contains(QStringLiteral("error"))) {
            result.insert(QStringLiteral("error"), reply->errorString());
        }
        callback(result, isTransient(reply->error()));
    });
}

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
    emit stateChanged();
}

void HarnessBridge::sendNext() {
    if (m_requestInFlight || m_queue.isEmpty() || m_retryTimer.isActive()) {
        if (!m_requestInFlight && m_queue.isEmpty() && !m_retryTimer.isActive() && m_suggestionsDirty) {
            requestSuggestions();
        }
        return;
    }
    m_requestInFlight = true;
    const Request& request = m_queue.first();
    call(request.path, request.body, kRequestTimeoutMillis, this,
            [this](const QJsonObject& reply, bool transient) {
        m_requestInFlight = false;
        if (m_queue.isEmpty()) {
            return;
        }
        const QString error = reply.value(QStringLiteral("error")).toString();
        if (!error.isEmpty() && transient) {
            // Keep it at the head: order matters (a rating needs its play).
            scheduleRetry(error);
            return;
        }
        Request done = m_queue.takeFirst();
        if (!error.isEmpty()) {
            // Refused as invalid: retrying would be refused again.
            kLogger.warning() << done.path << "rejected:" << error;
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
        emit stateChanged();
    });
}

void HarnessBridge::scheduleRetry(const QString& reason) {
    if (m_status != Status::Offline) {
        kLogger.warning() << "Harness unreachable:" << reason;
    }
    setStatus(Status::Offline, reason);
    m_retryTimer.start(m_retryDelayMillis);
    m_retryDelayMillis = std::min(m_retryDelayMillis * 2, kMaxRetryMillis);
}

void HarnessBridge::probeHealth() {
    call(QStringLiteral("/health"), {}, kRequestTimeoutMillis, this,
            [this](const QJsonObject& reply, bool) {
        if (reply.contains(QStringLiteral("error"))) {
            scheduleRetry(reply.value(QStringLiteral("error")).toString());
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
    if (m_suggestionsInFlight || m_requestInFlight || !m_queue.isEmpty()) {
        // Something changed while the last request was out; ask again after.
        m_suggestionsDirty = true;
        return;
    }
    m_suggestionsInFlight = true;
    m_suggestionsDirty = false;
    QJsonObject body{
            {QStringLiteral("session"), m_session},
            {QStringLiteral("count"), kSuggestionCount},
    };
    for (auto it = m_agentAction.constBegin(); it != m_agentAction.constEnd(); ++it) {
        body.insert(it.key(), it.value());
    }
    m_agentAction = QJsonObject();
    const QString requestedSession = m_session;
    emit stateChanged();
    call(QStringLiteral("/api/agent"), body, kSuggestionTimeoutMillis, this,
            [this, requestedSession](const QJsonObject& reply, bool transient) {
        m_suggestionsInFlight = false;
        if (m_suggestionsDirty || requestedSession != m_session) {
            requestSuggestions();
            return;
        }
        if (reply.contains(QStringLiteral("error"))) {
            if (transient) {
                scheduleRetry(reply.value(QStringLiteral("error")).toString());
            } else {
                kLogger.warning() << "Suggestions rejected:" << reply.value(QStringLiteral("error")).toString();
                setStatus(Status::Heuristic, tr("Agent request failed"));
            }
            emit stateChanged();
            return;
        }
        m_agentPlan = reply.value(QStringLiteral("plan")).toObject();
        m_rankedSuggestions.clear();
        const QJsonArray tracks = reply.value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& value : tracks) {
            if (m_rankedSuggestions.size() >= kSuggestionCount) {
                break;
            }
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
                QStringList reasons;
                for (const auto& reason : track.value(QStringLiteral("reasons")).toArray()) {
                    reasons.append(reason.toString());
                }
                suggestion.reason = reasons.join(QStringLiteral(" · "));
            } else {
                suggestion.reason.prepend(tr("Model: "));
            }
            m_rankedSuggestions.append(suggestion);
        }
        const QString modelError = reply.value(QStringLiteral("model_error")).toString();
        if (reply.value(QStringLiteral("source")).toString() == QStringLiteral("model")) {
            setStatus(Status::Model, reply.value(QStringLiteral("summary")).toString());
        } else {
            setStatus(Status::Heuristic, modelError);
        }
        QString summary = reply.value(QStringLiteral("summary")).toString();
        if (summary.isEmpty()) {
            summary = m_rankedSuggestions.isEmpty() ? tr("No eligible songs found. Add or analyze more tracks.")
                    : tr("Next: %1 — %2").arg(m_rankedSuggestions.first().title, m_rankedSuggestions.first().reason);
        }
        if (!modelError.isEmpty()) {
            summary = tr("Using local recommendations: %1\n%2").arg(modelError, summary);
        }
        m_adviceSummary = summary;
        QJsonArray picks;
        for (const auto& song : m_rankedSuggestions) {
            picks.append(QJsonObject{{"id", song.trackId}, {"reason", song.reason}});
        }
        const QByteArray fingerprint = QJsonDocument(QJsonObject{
                {"session", requestedSession}, {"summary", summary}, {"picks", picks}}).toJson(QJsonDocument::Compact);
        if (fingerprint != m_adviceFingerprint) {
            m_adviceFingerprint = fingerprint;
            emit adviceReceived(summary);
        }
        mergeSuggestions();
        emit stateChanged();
        if (m_suggestionsDirty) {
            requestSuggestions();
        }
    });
}

void HarnessBridge::mergeSuggestions() {
    m_suggestions.clear();
    QSet<QString> seen;
    for (const auto& source : {m_generatedSuggestions, m_rankedSuggestions}) {
        for (const auto& suggestion : source) {
            if (m_suggestions.size() >= kSuggestionCount) {
                break;
            }
            if (!seen.contains(suggestion.trackId) && !m_consumedMusic.contains(suggestion.path)) {
                seen.insert(suggestion.trackId);
                m_suggestions.append(suggestion);
            }
        }
    }
    m_pCoSuggestionCount->forceSet(m_suggestions.size());
    emit stateChanged();
}

void HarnessBridge::consumeGenerated(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    bool generated = false;
    for (const auto& song : m_generatedSuggestions) {
        generated |= song.path == path;
    }
    if (!generated || m_consumedMusic.contains(path)) {
        return;
    }
    m_consumedMusic.insert(path);
    enqueue({QStringLiteral("/api/agent/music/consume"), {{QStringLiteral("path"), path}},
            [this](const QJsonObject&) { pollGeneratedMusic(); }});
    mergeSuggestions();
}

void HarnessBridge::pollGeneratedMusic() {
    if (!m_enabled || !m_pTrackCollectionManager || m_musicInFlight) {
        return;
    }
    m_musicInFlight = true;
    call(QStringLiteral("/api/agent/music/view"), {}, kRequestTimeoutMillis, this,
            [this](const QJsonObject& reply, bool) {
        m_musicInFlight = false;
        if (reply.contains(QStringLiteral("error"))) {
            return;
        }
        const auto pending = reply.value(QStringLiteral("upcoming")).toArray();
        const auto completed = reply.contains(QStringLiteral("completed"))
                ? reply.value(QStringLiteral("completed")).toArray() : pending;
        bool imported = false;
        for (const auto& value : completed) {
            const auto job = value.toObject();
            const QString path = job.value(QStringLiteral("path")).toString();
            if (!QFileInfo::exists(path)) {
                continue;
            }
            const QString title = job.value(QStringLiteral("title")).toString();
            if (!m_importedMusic.contains(path)) {
                auto track = m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
                if (!track) {
                    publish(tr("Could not import generated song: %1").arg(title), true);
                    continue;
                }
                track->setArtist(QStringLiteral("ElevenLabs"));
                track->setTitle(title);
                if (m_pTrackCollectionManager->saveTrack(track) == TrackCollectionManager::SaveTrackResult::Failed) {
                    continue;
                }
                auto& playlists = m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
                int playlist = playlists.getPlaylistIdFromName(QStringLiteral("ElevenLabs"));
                if (playlist < 0) playlist = playlists.createPlaylist(QStringLiteral("ElevenLabs"));
                if (playlist < 0 || (!playlists.getTrackIds(playlist).contains(track->getId()) &&
                                           !playlists.appendTrackToPlaylist(track->getId(), playlist))) {
                    continue;
                }
                m_importedMusic.insert(path);
                imported = true;
            }
        }
        QList<Suggestion> upcoming;
        for (const auto& value : pending) {
            const auto job = value.toObject();
            const QString path = job.value(QStringLiteral("path")).toString();
            if (m_consumedMusic.contains(path) || !m_importedMusic.contains(path) || !QFileInfo::exists(path)) continue;
            Suggestion song;
            song.trackId = mixxx::harness::trackIdForLocation(path, mountedDrives());
            song.title = job.value(QStringLiteral("title")).toString();
            song.artist = QStringLiteral("ElevenLabs");
            song.path = path;
            song.reason = tr("Newly generated · added to library · tempo/key pending analysis");
            upcoming.append(song);
        }
        QStringList before, after;
        for (const auto& song : m_generatedSuggestions) before.append(song.path);
        for (const auto& song : upcoming) after.append(song.path);
        m_generatedSuggestions = upcoming;
        if (before != after) {
            mergeSuggestions();
        }
        if (imported) syncDrives();
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

QJsonArray HarnessBridge::catalogForDrive(const mixxx::harness::Drive& drive) const {
    QJsonArray tracks;
    if (!m_pTrackCollectionManager) {
        return tracks;
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    // rekordbox_library is the fork's mirror of every Rekordbox export on a
    // mounted drive (RekordboxFeature); it may not exist yet.
    const bool local = drive.mountPoint.isEmpty();
    if (local) {
        query.prepare(QStringLiteral(
                "SELECT track_locations.location, library.title, library.artist, library.genre, "
                "library.duration, library.bpm, library.key FROM library "
                "JOIN track_locations ON library.location=track_locations.id "
                "WHERE library.mixxx_deleted=0 AND track_locations.fs_deleted=0 "
                "ORDER BY track_locations.location"));
    } else {
        query.prepare(QStringLiteral(
                "SELECT location, title, artist, genre, duration, bpm, key "
                "FROM rekordbox_library WHERE substr(location, 1, :length) = :prefix ORDER BY location"));
        const QString prefix = drive.mountPoint + QLatin1Char('/');
        query.bindValue(QStringLiteral(":length"), prefix.size());
        query.bindValue(QStringLiteral(":prefix"), prefix);
    }
    if (!query.exec()) {
        return tracks;
    }
    const QList<mixxx::harness::Drive> drives = local ? mountedDrives() : QList<mixxx::harness::Drive>{drive};
    while (query.next()) {
        const QString location = query.value(0).toString();
        if ((local && mixxx::harness::driveForLocation(location, drives)) || !QFileInfo::exists(location)) {
            continue;
        }
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

    // An empty mount denotes the local Mixxx collection. Keep it out of
    // m_scopeByMount so drive eject handling never treats it as removable.
    scopeByMount.insert(QString(), QStringLiteral("local"));

    for (auto it = scopeByMount.cbegin(); it != scopeByMount.cend(); ++it) {
        const QString scope = it.value();
        const mixxx::harness::Drive drive = driveForScope(it.key(), scope);
        const QJsonArray catalog = catalogForDrive(drive);
        const QByteArray digest = QCryptographicHash::hash(
                QJsonDocument(catalog).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        if (m_syncedCatalogByScope.value(scope) == digest) {
            continue;
        }
        const int count = catalog.size();
        const QJsonObject body{
                {QStringLiteral("scope"), scope},
                {QStringLiteral("complete"), true},
                {QStringLiteral("tracks"), catalog},
        };
        m_syncedCatalogByScope.insert(scope, digest);
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
    m_syncedCatalogByScope.remove(scope);
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
