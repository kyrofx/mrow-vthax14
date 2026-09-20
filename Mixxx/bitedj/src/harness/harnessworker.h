#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <functional>

class QTimer;

/// The agent ships as Qt resources inside Mixxx. Python is an implementation
/// detail of this owned worker, not a service or a listening network endpoint.
class HarnessWorker : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const QJsonObject&, bool transient)>;
    explicit HarnessWorker(QString database, QObject* parent = nullptr);
    ~HarnessWorker() override;
    void request(const QString& path, const QJsonObject& body, int timeoutMillis,
            QObject* context, Callback callback);

  signals:
    void stopped(const QString& reason);

  private:
    struct Pending {
        QPointer<QObject> context;
        Callback callback;
        QTimer* timer;
        QByteArray message;
        bool sent = false;
    };
    void start();
    void readOutput();
    void flush();
    void finish(qint64 id, const QJsonObject& body, bool transient);
    void failed(const QString& reason);

    QString m_database;
    QTemporaryDir m_code;
    QProcess m_process;
    QByteArray m_output;
    QMap<qint64, Pending> m_pending;
    qint64 m_nextId = 0;
    bool m_ready = false;
};
