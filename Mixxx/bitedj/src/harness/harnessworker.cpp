#include "harness/harnessworker.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>

#include "moc_harnessworker.cpp"

HarnessWorker::HarnessWorker(QString database, QObject* parent)
        : QObject(parent), m_database(std::move(database)) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &HarnessWorker::readOutput);
    // Do not forward arbitrary Python output into Mixxx's log: keys must not
    // appear there even if a provider or runtime emits an unexpected message.
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_process.readAllStandardError();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failed(tr("Could not start the built-in agent. Python 3.9+ is required."));
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        failed(tr("Agent stopped; restarting. Runtime keys need reentry; provisioned keys reload."));
    });
}

HarnessWorker::~HarnessWorker() {
    disconnect(&m_process, nullptr, this, nullptr);
    m_process.terminate();
    if (!m_process.waitForFinished(300)) {
        m_process.kill();
        m_process.waitForFinished(300);
    }
}

void HarnessWorker::start() {
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }
    if (!m_code.isValid()) {
        failed(tr("Could not unpack the built-in agent."));
        return;
    }
    for (const QString& name : {QStringLiteral("worker.py"), QStringLiteral("harness.py"),
                 QStringLiteral("music.py"), QStringLiteral("agent.py"), QStringLiteral("model.py"), QStringLiteral("scoring.py")}) {
        QFile resource(QStringLiteral(":/agent/") + name);
        QFile target(QDir(m_code.path()).filePath(name));
        if (!resource.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            failed(tr("The Mixxx build is missing its embedded agent."));
            return;
        }
        const QByteArray bytes = resource.readAll();
        if (target.write(bytes) != bytes.size()) {
            failed(tr("Could not unpack the built-in agent."));
            return;
        }
        target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        failed(tr("Python 3.9+ is required for the built-in agent."));
        return;
    }
    m_ready = false;
    m_output.clear();
    // -E ignores PYTHON* environment overrides; -s excludes user site packages.
    m_process.start(python, {QStringLiteral("-E"), QStringLiteral("-s"), QStringLiteral("-u"),
                                   QDir(m_code.path()).filePath(QStringLiteral("worker.py")),
                                   QStringLiteral("--database"), m_database});
}

void HarnessWorker::request(const QString& path, const QJsonObject& body, int timeoutMillis,
        QObject* context, Callback callback) {
    const qint64 id = ++m_nextId;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id] {
        finish(id, QJsonObject{{QStringLiteral("error"), tr("Agent request timed out")}}, true);
    });
    const QByteArray message = QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                                     {QStringLiteral("path"), path},
                                                     {QStringLiteral("body"), body}})
                                       .toJson(QJsonDocument::Compact) + '\n';
    m_pending.insert(id, Pending{context, std::move(callback), timer, message});
    timer->start(timeoutMillis);
    start();
    flush();
}

void HarnessWorker::flush() {
    if (!m_ready) {
        return;
    }
    for (auto& pending : m_pending) {
        if (!pending.sent) {
            m_process.write(pending.message);
            pending.message.clear(); // Includes runtime keys on settings requests.
            pending.sent = true;
        }
    }
}

void HarnessWorker::readOutput() {
    m_output += m_process.readAllStandardOutput();
    if (m_output.size() > 32 * 1024 * 1024) {
        m_process.kill();
        return;
    }
    qsizetype end;
    while ((end = m_output.indexOf('\n')) >= 0) {
        const QJsonObject response = QJsonDocument::fromJson(m_output.left(end)).object();
        m_output.remove(0, end + 1);
        if (response.value(QStringLiteral("ready")).toBool()) {
            m_ready = true;
            flush();
        } else if (response.contains(QStringLiteral("id"))) {
            const int status = response.value(QStringLiteral("status")).toInt(500);
            QJsonObject body = response.value(QStringLiteral("body")).toObject();
            if (status >= 400 && !body.contains(QStringLiteral("error"))) {
                body.insert(QStringLiteral("error"), tr("Agent command failed"));
            }
            finish(response.value(QStringLiteral("id")).toInteger(), body, status >= 500);
        }
    }
}

void HarnessWorker::finish(qint64 id, const QJsonObject& body, bool transient) {
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    const Pending pending = m_pending.take(id);
    pending.timer->stop();
    pending.timer->deleteLater();
    if (pending.context) {
        pending.callback(body, transient);
    }
}

void HarnessWorker::failed(const QString& reason) {
    m_ready = false;
    const auto ids = m_pending.keys();
    for (qint64 id : ids) {
        finish(id, QJsonObject{{QStringLiteral("error"), reason}}, true);
    }
    emit stopped(reason);
}
