#pragma once

#include <QAtomicPointer>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

#include "harness/harnessids.h"
#include "harness/harnessworker.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlObject;
class ControlProxy;
class QNetworkReply;
class PlayerManager;
class TrackCollectionManager;

/// Bite DJ: owns the bundled MROW agent worker, which keeps play history and the
/// DJ's crowd ratings in ~/.mixxx/harness/ and suggests what to play next,
/// optionally refined by a cloud model.
///
/// The GUI talks asynchronously over private process pipes (no web server).
/// Nothing about audio waits on it, and the
/// harness being down (or the model being unreachable) degrades the Assist
/// panel, never the set.
///
/// - Plays: a track becoming the audible one (PlayerInfo) is reported with the
///   deck's actual BPM, using the same six-track replay window as the History
///   feature so the two records agree.
/// - Ratings: `[Harness],rate_good|rate_mid|rate_bad` rate the current play.
///   Every input maps onto these controls: the Assist panel, the GPIO crowd
///   buttons (through their MIDI bridge), or any controller mapping.
/// - Suggestions: fetched after every play and rating. They are suggestions
///   only; a track is loaded when the DJ asks for it, never into a playing
///   deck.
/// - Library: the Rekordbox catalog of each mounted drive is synced so the
///   harness can suggest from the whole stick; an ejected drive's tracks stop
///   being suggested but keep their history.
///
/// Plays, ratings and syncs are queued and retried in order while the harness
/// is unreachable, and each play carries a stable event id, so a retry never
/// records a play twice.
class HarnessBridge : public QObject {
    Q_OBJECT
  public:
    /// Suggestions shown, and the `[Harness],*_suggestion_N` controls created.
    /// Four is what the 800x480 panel fits below the topbar at the 56px touch
    /// row height; a fifth row would be cut off.
    static constexpr int kSuggestionCount = 4;
    /// Decks a suggestion can be loaded into from the panel and controls.
    static constexpr int kLoadDeckCount = 2;

    enum class Status {
        /// The harness has not answered yet, or stopped answering.
        Offline = 0,
        /// Suggestions come from the harness's own ranking (no model, or the
        /// model could not be reached this time).
        Heuristic = 1,
        /// Suggestions were reordered by the cloud model.
        Model = 2,
    };

    enum class Rating {
        None = 0,
        Bad = 1,
        Mid = 2,
        Good = 3,
    };

    struct Suggestion {
        QString trackId;
        QString title;
        QString artist;
        QString key;
        double bpm = 0;
        /// The model's reason when it picked this one, else the heuristic's.
        QString reason;
        /// Absolute path when last synced; the current location is resolved
        /// from the track id at load time, since a drive may have moved.
        QString path;
    };

    /// What was played, as reported to the harness.
    struct Play {
        QString trackId;
        QString scope;
        QString title;
        QString artist;
        QString genre;
        QString key;
        QString location;
        QString group;
        double libraryBpm = 0;
        double bpm = 0;
        double durationSeconds = 0;
    };

    /// `pPlayerManager` and `pTrackCollectionManager` may be null (tests):
    /// loading and library sync are then unavailable.
    HarnessBridge(UserSettingsPointer pConfig,
            std::shared_ptr<PlayerManager> pPlayerManager,
            std::shared_ptr<TrackCollectionManager> pTrackCollectionManager);
    ~HarnessBridge() override;

    /// Owned by CoreServices. Null in builds and tests that never construct one,
    /// which is what keeps WHarnessPanel optional.
    static HarnessBridge* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    Status status() const {
        return m_status;
    }
    /// Short explanation for the panel: why the harness or model is offline.
    QString statusDetail() const {
        return m_statusDetail;
    }
    QString session() const {
        return m_session;
    }
    QList<Suggestion> suggestions() const {
        return m_suggestions;
    }
    /// "Artist - Title" of the play being rated; empty before the first play.
    QString currentTrackLabel() const;
    Rating currentRating() const {
        return m_current.rating;
    }

    void rate(Rating rating);
    /// Hide the suggestion at `index` (0-based) for the rest of this set.
    void skipSuggestion(int index);
    /// Load the suggestion at `index` (0-based) into deck `deckNumber`
    /// (1-based). Refused, with a notification, when that deck is playing or
    /// the track's drive is not mounted.
    void loadSuggestion(int index, int deckNumber);
    void refreshSuggestions();
    /// Shared rolling setlist and runtime model controls for the native panel.
    QJsonObject agentPlan() const { return m_agentPlan; }
    bool agentBusy() const { return m_suggestionsInFlight || m_requestInFlight || !m_queue.isEmpty(); }
    void planSet(int count, const QString& direction, bool clear = false);
    void loadPlanTrack(int index, int deckNumber);
    void agentRequest(const QString& path, const QJsonObject& body, QObject* context,
            std::function<void(const QJsonObject&)> callback);
    /// Start a new set: new session name, empty "played" list in the harness.
    void startNewSession();

    /// Report a play. Called for PlayerInfo::currentPlayingTrackChanged after
    /// the replay-window check; public so tests can drive it without decks.
    void reportPlay(const Play& play);

    /// Re-read the mounted drives and sync any whose Rekordbox catalog changed.
    void syncDrives();

  signals:
    /// Suggestions, current play, rating or status changed. WHarnessPanel
    /// rebuilds from the getters.
    void stateChanged();

  private:
    struct Request {
        QString path;
        QJsonObject body;
        std::function<void(const QJsonObject& reply)> onSuccess;
    };

    struct CurrentPlay {
        QString trackId;
        QString label;
        QString eventId;
        /// 0 until the harness acknowledges the play.
        qint64 playId = 0;
        Rating rating = Rating::None;
        /// A rating made before the play was acknowledged, sent once it is.
        bool ratingPending = false;
    };

    void onCurrentPlayingTrackChanged(TrackPointer pTrack);
    void onMountEjected(const QString& mountPoint);
    void setupControls();

    /// Queue a request that must reach the harness (play, rating, sync), in
    /// order, retrying while it is unreachable.
    void enqueue(Request request);
    void sendNext();
    void scheduleRetry(const QString& reason);
    void probeHealth();
    void requestSuggestions();
    void loadTrack(const Suggestion& suggestion, int deckNumber);
    QNetworkReply* post(const QString& path, const QJsonObject& body, int timeoutMillis);
    void call(const QString& path, const QJsonObject& body, int timeoutMillis,
            QObject* context, HarnessWorker::Callback callback);

    void sendRating();
    void setStatus(Status status, const QString& detail);
    void setCurrentRating(Rating rating);
    QList<mixxx::harness::Drive> mountedDrives() const;
    /// The drive's catalog as the harness sync expects it.
    QJsonArray catalogForDrive(const mixxx::harness::Drive& drive) const;
    void publish(const QString& message, bool warning) const;

    static QAtomicPointer<HarnessBridge> s_pInstance;

    UserSettingsPointer m_pConfig;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::shared_ptr<TrackCollectionManager> m_pTrackCollectionManager;

    bool m_enabled;
    QUrl m_baseUrl;
    QNetworkAccessManager m_network;
    std::unique_ptr<HarnessWorker> m_worker;

    QString m_session;
    Status m_status;
    QString m_statusDetail;
    QList<Suggestion> m_suggestions;
    QJsonObject m_agentPlan;
    QJsonObject m_agentAction;
    CurrentPlay m_current;
    /// Locations of the most recent distinct plays, newest first. A track
    /// replayed within this window is not a new play (matches SetlogFeature).
    QStringList m_recentLocations;

    QList<Request> m_queue;
    bool m_requestInFlight;
    QTimer m_retryTimer;
    int m_retryDelayMillis;
    bool m_suggestionsInFlight;
    bool m_suggestionsDirty;

    QTimer m_syncTimer;
    /// Scope of each mounted drive, by mount point: an ejected drive can no
    /// longer be asked for its UUID, so it is remembered here.
    QHash<QString, QString> m_scopeByMount;
    /// Content fingerprint last synced per scope, including analyzed metadata.
    QHash<QString, QByteArray> m_syncedCatalogByScope;

    std::vector<std::unique_ptr<ControlObject>> m_controls;
    ControlObject* m_pCoStatus;
    ControlObject* m_pCoCurrentRating;
    ControlObject* m_pCoSuggestionCount;
    std::unique_ptr<ControlProxy> m_pResetPlayedTracks;
};
