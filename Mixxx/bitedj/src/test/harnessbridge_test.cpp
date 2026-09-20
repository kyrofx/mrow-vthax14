// Tests for the Bite DJ link to the MROW DJ agent: how tracks, drives
// and sets are named (harnessids), and the bridge's conversation with a fake
// harness — plays, ratings made before a play is acknowledged, retries while
// the harness is down, suggestions, skips, and starting a new set.
#include "harness/harnessbridge.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTest>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "harness/harnessids.h"
#include "mixer/playerinfo.h"
#include "test/librarytest.h"

using namespace mixxx::harness;
using mixxx::track::io::key::ChromaticKey;

namespace {

constexpr int kWaitMillis = 5000;

TEST(HarnessWorkerTest, EmbeddedWorkerStartsWithoutServerAndKeepsKeyPrivate) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    HarnessWorker worker(temporary.filePath(QStringLiteral("agent.sqlite3")));
    QObject context;
    QJsonObject result;
    bool received = false;
    auto call = [&](const QString& path, const QJsonObject& body) {
        received = false;
        worker.request(path, body, 5000, &context,
                [&](const QJsonObject& reply, bool transient) {
                    EXPECT_FALSE(transient);
                    result = reply;
                    received = true;
                });
        return QTest::qWaitFor([&] { return received; }, 6000);
    };
    ASSERT_TRUE(call(QStringLiteral("/health"), {}));
    EXPECT_TRUE(result.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(call(QStringLiteral("/api/agent/settings"),
            {{"api_key", "private-test-key"}, {"next_model", "test/quick"}, {"plan_model", "test/plan"}}));
    EXPECT_TRUE(result.value(QStringLiteral("connected")).toBool());
    EXPECT_FALSE(QJsonDocument(result).toJson().contains("private-test-key"));
    ASSERT_TRUE(call(QStringLiteral("/api/agent/settings"), {{"disconnect", true}}));
    EXPECT_FALSE(result.value(QStringLiteral("connected")).toBool());
    ASSERT_TRUE(call(QStringLiteral("/api/agent"), {{"session", "native"}}));
    EXPECT_FALSE(result.contains(QStringLiteral("error")));
    QFile database(temporary.filePath(QStringLiteral("agent.sqlite3")));
    ASSERT_TRUE(database.open(QIODevice::ReadOnly));
    EXPECT_FALSE(database.readAll().contains("private-test-key"));
}

// ---- harnessids --------------------------------------------------------------

const QList<Drive> kDrives{
        {QStringLiteral("/media/dj/STICK"), QStringLiteral("1234-ABCD")},
        {QStringLiteral("/media/dj/STICK/nested"), QStringLiteral("NESTED")},
        {QStringLiteral("/media/dj/NOUUID"), QString()},
};

TEST(HarnessIdsTest, DriveTracksAreKeyedByUuidAndRelativePath) {
    EXPECT_EQ(QStringLiteral("1234-ABCD:Music/a b.mp3"),
            trackIdForLocation(QStringLiteral("/media/dj/STICK/Music/a b.mp3"), kDrives));
    // The longest mount wins.
    EXPECT_EQ(QStringLiteral("NESTED:x.mp3"),
            trackIdForLocation(QStringLiteral("/media/dj/STICK/nested/x.mp3"), kDrives));
    // A sibling whose name merely starts like a mount point is not on it.
    EXPECT_EQ(QStringLiteral("local:/media/dj/STICKY/x.mp3"),
            trackIdForLocation(QStringLiteral("/media/dj/STICKY/x.mp3"), kDrives));
    EXPECT_EQ(QStringLiteral("mount:/media/dj/NOUUID:x.mp3"),
            trackIdForLocation(QStringLiteral("/media/dj/NOUUID/x.mp3"), kDrives));
    EXPECT_EQ(QStringLiteral("local:/home/dj/x.mp3"),
            trackIdForLocation(QStringLiteral("/home/dj/x.mp3"), kDrives));
}

TEST(HarnessIdsTest, TrackIdsFollowADriveToItsNewMountPoint) {
    const QString trackId = trackIdForLocation(QStringLiteral("/media/dj/STICK/Music/a.mp3"), kDrives);
    const QList<Drive> replugged{{QStringLiteral("/media/dj/STICK1"), QStringLiteral("1234-ABCD")}};
    EXPECT_EQ(QStringLiteral("/media/dj/STICK1/Music/a.mp3"), locationForTrackId(trackId, replugged));
    EXPECT_TRUE(locationForTrackId(trackId, {}).isEmpty());
    EXPECT_EQ(QStringLiteral("/media/dj/NOUUID/x.mp3"),
            locationForTrackId(QStringLiteral("mount:/media/dj/NOUUID:x.mp3"), kDrives));
    EXPECT_EQ(QStringLiteral("/home/dj/x.mp3"), locationForTrackId(QStringLiteral("local:/home/dj/x.mp3"), {}));
}

TEST(HarnessIdsTest, CamelotKeys) {
    EXPECT_EQ(QStringLiteral("8A"), camelotForKey(ChromaticKey::A_MINOR));
    EXPECT_EQ(QStringLiteral("8B"), camelotForKey(ChromaticKey::C_MAJOR));
    EXPECT_EQ(QStringLiteral("1A"), camelotForKey(ChromaticKey::G_SHARP_MINOR));
    EXPECT_EQ(QStringLiteral("10A"), camelotForKey(ChromaticKey::B_MINOR));
    EXPECT_TRUE(camelotForKey(ChromaticKey::INVALID).isEmpty());
    // Spellings a Rekordbox export or a tag may use.
    EXPECT_EQ(QStringLiteral("8A"), camelotForKeyText(QStringLiteral("Am")));
    EXPECT_EQ(QStringLiteral("11A"), camelotForKeyText(QStringLiteral("F#m")));
    EXPECT_EQ(QStringLiteral("8A"), camelotForKeyText(QStringLiteral("8A")));
    EXPECT_TRUE(camelotForKeyText(QString()).isEmpty());
}

TEST(HarnessIdsTest, SessionNames) {
    const QDate today(2026, 9, 19);
    EXPECT_EQ(QStringLiteral("2026-09-19"), nextSessionName(today, QString()));
    EXPECT_EQ(QStringLiteral("2026-09-19"), nextSessionName(today, QStringLiteral("2026-09-18#3")));
    EXPECT_EQ(QStringLiteral("2026-09-19#2"), nextSessionName(today, QStringLiteral("2026-09-19")));
    EXPECT_EQ(QStringLiteral("2026-09-19#4"), nextSessionName(today, QStringLiteral("2026-09-19#3")));
    EXPECT_TRUE(isSessionFrom(QStringLiteral("2026-09-19#2"), today));
    EXPECT_FALSE(isSessionFrom(QStringLiteral("2026-09-18"), today));
}

// ---- A fake harness ----------------------------------------------------------

/// Minimal HTTP/1.1 server: records each request and answers from `replies`
/// by path ({} when unset), one request per connection.
class FakeHarness : public QObject {
  public:
    struct Received {
        QString method;
        QString path;
        QJsonObject body;
    };

    FakeHarness() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* pSocket = m_server.nextPendingConnection()) {
                connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket] { onData(pSocket); });
                connect(pSocket, &QTcpSocket::disconnected, pSocket, &QObject::deleteLater);
            }
        });
    }

    bool listen(quint16 port = 0) {
        return m_server.listen(QHostAddress::LocalHost, port);
    }
    void close() {
        m_server.close();
    }
    quint16 port() const {
        return m_server.serverPort();
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(port());
    }

    QList<Received> requests(const QString& path) const {
        QList<Received> result;
        for (const Received& request : m_received) {
            if (request.path == path) {
                result.append(request);
            }
        }
        return result;
    }

    QHash<QString, QJsonObject> replies;

  private:
    void onData(QTcpSocket* pSocket) {
        QByteArray& buffer = m_buffers[pSocket];
        buffer += pSocket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        int length = 0;
        for (const QByteArray& line : lines) {
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(15).trimmed().toInt();
            }
        }
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        Received received{QString::fromLatin1(requestLine.value(0)),
                QString::fromLatin1(requestLine.value(1)),
                QJsonDocument::fromJson(buffer.mid(headerEnd + 4, length)).object()};
        m_received.append(received);
        m_buffers.remove(pSocket);

        const QByteArray body = QJsonDocument(replies.value(received.path)).toJson(QJsonDocument::Compact);
        pSocket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                       "Content-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
        pSocket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<Received> m_received;
};

HarnessBridge::Play play(const QString& trackId) {
    HarnessBridge::Play result;
    result.trackId = trackId;
    result.scope = QStringLiteral("1234-ABCD");
    result.title = QStringLiteral("Song");
    result.artist = QStringLiteral("Artist");
    result.location = QStringLiteral("/media/dj/STICK/") + trackId;
    result.group = QStringLiteral("[Channel1]");
    result.bpm = 124.5;
    return result;
}

class HarnessBridgeTest : public LibraryTest {
  protected:
    HarnessBridgeTest()
            // Library creates this in the app; the bridge follows it.
            : m_resetPlayedTracks(ConfigKey(QStringLiteral("[Library]"),
                      QStringLiteral("reset_played_tracks"))),
              m_crossfader(ConfigKey("[Master]", "crossfader")),
              m_numDecks(ConfigKey("[App]", "num_decks")),
              m_numSamplers(ConfigKey("[App]", "num_samplers")),
              m_numPreviewDecks(ConfigKey("[App]", "num_preview_decks")) {
        // No PlayerManager in this fixture: advertise zero audio decks so
        // PlayerInfo's polling timer does not request nonexistent controls.
        PlayerInfo::create();
        m_harness.replies.insert(QStringLiteral("/health"), QJsonObject{{"ok", true}});
        m_harness.replies.insert(QStringLiteral("/api/play"), QJsonObject{{"play_id", 41}});
        m_harness.replies.insert(QStringLiteral("/api/agent"),
                QJsonObject{{"source", "model"},
                        {"tracks",
                                QJsonArray{QJsonObject{{"id", "1234-ABCD:b.mp3"},
                                        {"title", "Next"},
                                        {"artist", "Someone"},
                                        {"bpm", 125.0},
                                        {"camelot", "8A"},
                                        {"model_reason", "Keeps the floor moving"},
                                        {"reasons", QJsonArray{"heuristic"}}}}}});
    }
    ~HarnessBridgeTest() override {
        m_pBridge.reset();
        PlayerInfo::destroy();
    }

    void startBridge(const QString& url = QString()) {
        config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("external")), true);
        config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("url")),
                url.isEmpty() ? m_harness.url() : url);
        m_pBridge = std::make_unique<HarnessBridge>(config(), nullptr, nullptr);
    }

    /// Pump the event loop until `condition` holds or the wait times out.
    template<typename Condition>
    bool waitFor(Condition condition) {
        return QTest::qWaitFor(condition, kWaitMillis);
    }

    FakeHarness m_harness;
    ControlPushButton m_resetPlayedTracks;
    ControlObject m_crossfader;
    ControlObject m_numDecks;
    ControlObject m_numSamplers;
    ControlObject m_numPreviewDecks;
    std::unique_ptr<HarnessBridge> m_pBridge;
};

TEST_F(HarnessBridgeTest, BuiltInAgentIgnoresLegacyUrlAndRecordsFeedback) {
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("external")), false);
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("url")),
            QStringLiteral("http://127.0.0.1:1"));
    m_pBridge = std::make_unique<HarnessBridge>(config(), nullptr, nullptr);
    ASSERT_TRUE(waitFor([&] { return m_pBridge->status() == HarnessBridge::Status::Heuristic; }));
    m_pBridge->reportPlay(play(QStringLiteral("local:/test/built-in.mp3")));
    m_pBridge->rate(HarnessBridge::Rating::Bad);
    ASSERT_TRUE(waitFor([&] { return !m_pBridge->agentBusy(); }));
    QJsonObject state;
    bool received = false;
    m_pBridge->agentRequest(QStringLiteral("/api/state"), {}, m_pBridge.get(),
            [&](const QJsonObject& reply) { state = reply; received = true; });
    ASSERT_TRUE(waitFor([&] { return received; }));
    EXPECT_FALSE(state.contains(QStringLiteral("error")));
    EXPECT_EQ(1, state.value(QStringLiteral("plays")).toArray().size());
    const auto feedback = state.value(QStringLiteral("feedback")).toArray();
    ASSERT_EQ(1, feedback.size());
    EXPECT_EQ(QStringLiteral("bad"), feedback.first().toObject().value(QStringLiteral("rating")).toString());
    EXPECT_TRUE(m_harness.requests(QStringLiteral("/health")).isEmpty());
}

TEST_F(HarnessBridgeTest, PlayRatingAndSuggestions) {
    ASSERT_TRUE(m_harness.listen());
    startBridge();
    ASSERT_TRUE(waitFor([&] { return m_pBridge->status() == HarnessBridge::Status::Model; }));

    auto currentPlay = play(QStringLiteral("1234-ABCD:a.mp3"));
    currentPlay.libraryBpm = 120;
    m_pBridge->reportPlay(currentPlay);
    // Rated before the harness has acknowledged the play: it must follow it.
    m_pBridge->rate(HarnessBridge::Rating::Good);
    EXPECT_EQ(3.0, ControlObject::get(ConfigKey("[Harness]", "current_rating")));
    ASSERT_TRUE(waitFor([&] { return !m_harness.requests("/api/feedback").isEmpty(); }));

    const auto plays = m_harness.requests("/api/play");
    ASSERT_EQ(1, plays.size());
    EXPECT_EQ(QStringLiteral("1234-ABCD:a.mp3"), plays.first().body.value("track_id").toString());
    EXPECT_EQ(124.5, plays.first().body.value("bpm").toDouble());
    EXPECT_EQ(120.0, plays.first().body.value("track").toObject().value("bpm").toDouble());
    EXPECT_EQ(m_pBridge->session(), plays.first().body.value("session").toString());
    EXPECT_FALSE(plays.first().body.value("event_id").toString().isEmpty());
    EXPECT_EQ(QStringLiteral("Song"),
            plays.first().body.value("track").toObject().value("title").toString());

    const QJsonObject feedback = m_harness.requests("/api/feedback").first().body;
    EXPECT_EQ(41, feedback.value("play_id").toInt());
    EXPECT_EQ(QStringLiteral("good"), feedback.value("rating").toString());

    ASSERT_EQ(1, m_pBridge->suggestions().size());
    const HarnessBridge::Suggestion suggestion = m_pBridge->suggestions().first();
    EXPECT_EQ(QStringLiteral("Model: Keeps the floor moving"), suggestion.reason);
    EXPECT_EQ(QStringLiteral("8A"), suggestion.key);
    EXPECT_EQ(1.0, ControlObject::get(ConfigKey("[Harness]", "suggestion_count")));
    EXPECT_EQ(2.0, ControlObject::get(ConfigKey("[Harness]", "status")));
}

TEST_F(HarnessBridgeTest, ControlsDriveRatingsAndSkips) {
    ASSERT_TRUE(m_harness.listen());
    startBridge();
    ASSERT_TRUE(waitFor([&] { return m_pBridge->suggestions().size() == 1; }));

    // Nothing has played: a rating is refused rather than guessed.
    ControlObject::set(ConfigKey("[Harness]", "rate_bad"), 1);
    ControlObject::set(ConfigKey("[Harness]", "rate_bad"), 0);
    EXPECT_EQ(HarnessBridge::Rating::None, m_pBridge->currentRating());

    m_pBridge->reportPlay(play(QStringLiteral("1234-ABCD:a.mp3")));
    // Acknowledged once the bridge asks for suggestions after it.
    ASSERT_TRUE(waitFor([&] { return m_harness.requests("/api/agent").size() >= 2; }));
    ControlObject::set(ConfigKey("[Harness]", "rate_bad"), 1);
    ControlObject::set(ConfigKey("[Harness]", "rate_bad"), 0);
    ControlObject::set(ConfigKey("[Harness]", "rate_mid"), 1);
    ASSERT_TRUE(waitFor([&] { return m_harness.requests("/api/feedback").size() == 2; }));
    EXPECT_EQ(QStringLiteral("mid"), m_harness.requests("/api/feedback").last().body.value("rating").toString());

    ASSERT_TRUE(waitFor([&] { return m_pBridge->suggestions().size() == 1; }));
    ControlObject::set(ConfigKey("[Harness]", "skip_suggestion_1"), 1);
    EXPECT_TRUE(m_pBridge->suggestions().isEmpty());
    ASSERT_TRUE(waitFor([&] { return m_harness.requests("/api/feedback").size() == 3; }));
    const QJsonObject skip = m_harness.requests("/api/feedback").last().body;
    EXPECT_EQ(QStringLiteral("skip"), skip.value("rating").toString());
    EXPECT_EQ(QStringLiteral("1234-ABCD:b.mp3"), skip.value("track_id").toString());
    EXPECT_FALSE(skip.contains("play_id"));
}

TEST_F(HarnessBridgeTest, PlaysAreQueuedWhileTheHarnessIsDown) {
    // Reserve a port, then stop listening: the harness is "not running yet".
    ASSERT_TRUE(m_harness.listen());
    const quint16 port = m_harness.port();
    const QString url = m_harness.url();
    m_harness.close();
    startBridge(url);

    m_pBridge->reportPlay(play(QStringLiteral("1234-ABCD:a.mp3")));
    m_pBridge->rate(HarnessBridge::Rating::Bad);
    ASSERT_TRUE(waitFor([&] { return m_pBridge->status() == HarnessBridge::Status::Offline; }));
    QTest::qWait(200);
    EXPECT_TRUE(m_harness.requests("/api/play").isEmpty());

    ASSERT_TRUE(m_harness.listen(port));
    // Reconnection waits out the retry backoff, which is capped at 15 s.
    ASSERT_TRUE(QTest::qWaitFor(
            [&] { return !m_harness.requests("/api/feedback").isEmpty(); }, 20000));
    ASSERT_EQ(1, m_harness.requests("/api/play").size());
    EXPECT_EQ(QStringLiteral("bad"), m_harness.requests("/api/feedback").first().body.value("rating").toString());
}

TEST_F(HarnessBridgeTest, RatingsBeforeTheAcknowledgementCollapse) {
    ASSERT_TRUE(m_harness.listen());
    startBridge();
    ASSERT_TRUE(waitFor([&] { return m_pBridge->status() == HarnessBridge::Status::Model; }));
    m_pBridge->reportPlay(play(QStringLiteral("1234-ABCD:a.mp3")));
    // Changed its mind before the play was acknowledged: only the last counts.
    m_pBridge->rate(HarnessBridge::Rating::Bad);
    m_pBridge->rate(HarnessBridge::Rating::Mid);
    ASSERT_TRUE(waitFor([&] { return !m_harness.requests("/api/feedback").isEmpty(); }));
    QTest::qWait(200);
    ASSERT_EQ(1, m_harness.requests("/api/feedback").size());
    EXPECT_EQ(QStringLiteral("mid"), m_harness.requests("/api/feedback").first().body.value("rating").toString());
}

TEST_F(HarnessBridgeTest, NewSetOnReset) {
    ASSERT_TRUE(m_harness.listen());
    startBridge();
    const QString first = m_pBridge->session();
    EXPECT_TRUE(isSessionFrom(first, QDate::currentDate()));
    m_pBridge->reportPlay(play(QStringLiteral("1234-ABCD:a.mp3")));

    m_resetPlayedTracks.set(1);
    EXPECT_NE(first, m_pBridge->session());
    EXPECT_TRUE(m_pBridge->currentTrackLabel().isEmpty());
    // The name is remembered, so a restart mid-set resumes it.
    EXPECT_EQ(m_pBridge->session(),
            config()->getValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("session")),
                    QString()));
}

TEST_F(HarnessBridgeTest, DisabledBridgeSendsNothing) {
    ASSERT_TRUE(m_harness.listen());
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("enabled")), false);
    startBridge();
    m_pBridge->reportPlay(play(QStringLiteral("1234-ABCD:a.mp3")));
    QTest::qWait(300);
    EXPECT_TRUE(m_harness.requests("/health").isEmpty());
    EXPECT_TRUE(m_harness.requests("/api/play").isEmpty());
    EXPECT_EQ(HarnessBridge::Status::Offline, m_pBridge->status());
}

TEST_F(HarnessBridgeTest, SetlistActionsAndRuntimeSettingsUseCurrentSession) {
    ASSERT_TRUE(m_harness.listen());
    startBridge();
    ASSERT_TRUE(waitFor([&] { return !m_pBridge->agentBusy() && m_pBridge->suggestions().size() == 1; }));
    QJsonObject response = m_harness.replies.value(QStringLiteral("/api/agent"));
    response.insert(QStringLiteral("plan"), QJsonObject{{"requested", 12}, {"basis", "test-basis"}});
    m_harness.replies.insert(QStringLiteral("/api/agent"), response);
    m_pBridge->planSet(12, QStringLiteral("up"));
    ASSERT_TRUE(waitFor([&] { return !m_pBridge->agentBusy() && !m_pBridge->agentPlan().isEmpty(); }));
    const auto request = m_harness.requests(QStringLiteral("/api/agent")).last().body;
    EXPECT_EQ(QStringLiteral("generate"), request.value("action").toString());
    EXPECT_EQ(12, request.value("count").toInt());
    EXPECT_EQ(QStringLiteral("up"), request.value("options").toObject().value("direction").toString());
    EXPECT_EQ(m_pBridge->session(), request.value("session").toString());
    EXPECT_EQ(QStringLiteral("test-basis"), m_pBridge->agentPlan().value("basis").toString());
    bool answered = false;
    m_pBridge->agentRequest(QStringLiteral("/api/agent/settings"),
            QJsonObject{{"disconnect", true}}, m_pBridge.get(), [&answered](const QJsonObject&) { answered = true; });
    ASSERT_TRUE(waitFor([&] { return answered; }));
    EXPECT_EQ(m_pBridge->session(),
            m_harness.requests(QStringLiteral("/api/agent/settings")).last().body.value("session").toString());
}

TEST_F(HarnessBridgeTest, LocalCatalogSyncDetectsMetadataEditsAndMissingFiles) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    QSqlQuery query(internalCollection()->database());
    query.prepare("INSERT INTO track_locations(location,fs_deleted) VALUES (:location,0)");
    query.bindValue(":location", file.fileName());
    ASSERT_TRUE(query.exec());
    const auto locationId = query.lastInsertId();
    query.prepare("INSERT INTO library(location,title,artist,bpm,key,duration,mixxx_deleted) "
                  "VALUES (:location,'Local song','Artist',124,'Am',180,0)");
    query.bindValue(":location", locationId);
    ASSERT_TRUE(query.exec());
    ASSERT_TRUE(m_harness.listen());
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("url")), m_harness.url());
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("external")), true);
    // The fixture owns the collection manager; the bridge only borrows it here.
    auto collection = std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {});
    m_pBridge = std::make_unique<HarnessBridge>(config(), nullptr, collection);
    ASSERT_TRUE(waitFor([&] { return !m_harness.requests("/api/library/sync").isEmpty(); }));
    auto body = m_harness.requests("/api/library/sync").last().body;
    EXPECT_EQ(QStringLiteral("local"), body.value("scope").toString());
    auto tracks = body.value("tracks").toArray();
    ASSERT_EQ(1, tracks.size());
    EXPECT_EQ(QStringLiteral("local:") + file.fileName(), tracks.first().toObject().value("id").toString());
    EXPECT_EQ(QStringLiteral("8A"), tracks.first().toObject().value("camelot").toString());

    ASSERT_TRUE(query.exec("UPDATE library SET bpm=128, title='Edited song'"));
    m_pBridge->syncDrives();
    ASSERT_TRUE(waitFor([&] { return m_harness.requests("/api/library/sync").size() >= 2; }));
    tracks = m_harness.requests("/api/library/sync").last().body.value("tracks").toArray();
    EXPECT_EQ(128.0, tracks.first().toObject().value("bpm").toDouble());
    EXPECT_EQ(QStringLiteral("Edited song"), tracks.first().toObject().value("title").toString());

    ASSERT_TRUE(file.remove());
    m_pBridge->syncDrives();
    ASSERT_TRUE(waitFor([&] { return m_harness.requests("/api/library/sync").size() >= 3; }));
    body = m_harness.requests("/api/library/sync").last().body;
    EXPECT_TRUE(body.value("complete").toBool());
    EXPECT_TRUE(body.value("tracks").toArray().isEmpty());
}

} // namespace

TEST_F(HarnessBridgeTest, GeneratedSongImportsAndQueuesWithoutOpenDialog) {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("generated.wav"));
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), path));
    ASSERT_TRUE(m_harness.listen());
    m_harness.replies.insert(QStringLiteral("/api/agent/music/view"),
            QJsonObject{{"upcoming", QJsonArray{QJsonObject{{"path", path}, {"title", "Crowd Mix test"}}}}});
    config()->setValue(ConfigKey("[Harness]", "external"), true);
    config()->setValue(ConfigKey("[Harness]", "url"), m_harness.url());
    auto collection = std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {});
    m_pBridge = std::make_unique<HarnessBridge>(config(), nullptr, collection);
    ASSERT_TRUE(waitFor([&] {
        return !m_pBridge->suggestions().isEmpty() &&
                m_pBridge->suggestions().first().artist == QStringLiteral("ElevenLabs");
    }));
    EXPECT_EQ(QStringLiteral("Crowd Mix test"), m_pBridge->suggestions().first().title);
    QSqlQuery query(internalCollection()->database());
    ASSERT_TRUE(query.exec("SELECT title,artist FROM library"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(QStringLiteral("Crowd Mix test"), query.value(0).toString());
    EXPECT_EQ(QStringLiteral("ElevenLabs"), query.value(1).toString());
    EXPECT_FALSE(query.next());
    EXPECT_EQ(12, m_harness.requests("/api/agent").last().body.value("count").toInt());
    m_pBridge->skipSuggestion(0);
    ASSERT_TRUE(waitFor([&] { return !m_harness.requests("/api/agent/music/consume").isEmpty(); }));
    EXPECT_EQ(path, m_harness.requests("/api/agent/music/consume").last().body.value("path").toString());
    for (const auto& song : m_pBridge->suggestions()) {
        EXPECT_NE(path, song.path);
    }
}
