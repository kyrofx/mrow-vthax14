#include "widget/wharnesspanel.h"

#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSaveFile>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "harness/harnessbridge.h"
#include "moc_wharnesspanel.cpp"
#include "skin/legacy/skincontext.h"

namespace {

const char* kStatusObjectName = "HarnessStatus";
const char* kNowPlayingObjectName = "HarnessNowPlaying";
const char* kRateButtonObjectName = "HarnessRateButton";
const char* kRefreshButtonObjectName = "HarnessRefreshButton";
const char* kSuggestionObjectName = "HarnessSuggestion";
const char* kLoadButtonObjectName = "HarnessLoadButton";
const char* kSkipButtonObjectName = "HarnessSkipButton";
const char* kEmptyObjectName = "HarnessEmpty";
// [selected] marks the rating given to the current play; [status] colours the
// status line (offline / heuristic / model).
const char* kSelectedProperty = "selected";
const char* kStatusProperty = "status";

// Columns: text, then Load 1, Load 2, Skip.
constexpr int kColumns = 4;
constexpr int kFirstSuggestionRow = 3;

void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}

QString statusName(HarnessBridge::Status status) {
    switch (status) {
    case HarnessBridge::Status::Model:
        return QStringLiteral("model");
    case HarnessBridge::Status::Heuristic:
        return QStringLiteral("heuristic");
    case HarnessBridge::Status::Offline:
        break;
    }
    return QStringLiteral("offline");
}

} // anonymous namespace

WHarnessPanel::WHarnessPanel(QWidget* parent)
        : WWidget(parent),
          m_pLayout(new QGridLayout(this)),
          m_pStatus(new QLabel(this)),
          m_pNowPlaying(new QLabel(this)),
          m_pressPending(false) {
    setAttribute(Qt::WA_StyledBackground, true);

    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setHorizontalSpacing(6);
    m_pLayout->setVerticalSpacing(6);
    m_pLayout->setColumnStretch(0, 1);
    // Keep rows pinned to the top instead of spreading down the panel.
    m_pLayout->setRowStretch(100, 1);

    m_pStatus->setObjectName(kStatusObjectName);
    m_pStatus->setTextFormat(Qt::PlainText);
    m_pStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_pLayout->addWidget(m_pStatus, 0, 0, 1, kColumns - 1);
    QPushButton* pRefresh = addButton(tr("Refresh"), kRefreshButtonObjectName, [] {
        if (HarnessBridge* pBridge = HarnessBridge::tryInstance()) {
            pBridge->refreshSuggestions();
        }
    });
    m_pLayout->addWidget(pRefresh, 0, kColumns - 1);

    // The play being rated, then the three crowd ratings beside it.
    m_pNowPlaying->setObjectName(kNowPlayingObjectName);
    m_pNowPlaying->setTextFormat(Qt::PlainText);
    m_pNowPlaying->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_pLayout->addWidget(m_pNowPlaying, 1, 0);
    const QList<QPair<QString, HarnessBridge::Rating>> ratings{
            {tr("Good"), HarnessBridge::Rating::Good},
            {tr("Mid"), HarnessBridge::Rating::Mid},
            {tr("Bad"), HarnessBridge::Rating::Bad},
    };
    for (int i = 0; i < ratings.size(); ++i) {
        const HarnessBridge::Rating rating = ratings.at(i).second;
        QPushButton* pButton = addButton(ratings.at(i).first, kRateButtonObjectName, [rating] {
            if (HarnessBridge* pBridge = HarnessBridge::tryInstance()) {
                pBridge->rate(rating);
            }
        });
        pButton->setProperty("rating", static_cast<int>(rating));
        m_pLayout->addWidget(pButton, 1, i + 1);
        m_rateButtons.append(pButton);
    }

    auto* pHeader = new QLabel(tr("Up next"), this);
    pHeader->setObjectName("SettingsHeader");
    m_pLayout->addWidget(pHeader, 2, 0);
    m_pLayout->addWidget(addButton(tr("Setlist"), kRefreshButtonObjectName,
                                [this] { showSetlist(); }), 2, 1);
    m_pLayout->addWidget(addButton(tr("Models"), kRefreshButtonObjectName,
                                [this] { showAgentSettings(); }), 2, 2);
    m_pLayout->addWidget(addButton(tr("New set"), kRefreshButtonObjectName, [] {
        if (auto* bridge = HarnessBridge::tryInstance()) {
            bridge->startNewSession();
        }
    }), 2, 3);

    if (HarnessBridge* pBridge = HarnessBridge::tryInstance()) {
        connect(pBridge, &HarnessBridge::stateChanged, this, &WHarnessPanel::onStateChanged);
    }
    onStateChanged();
}

void WHarnessPanel::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by HarnessBridge.
}

QPushButton* WHarnessPanel::addButton(
        const QString& text, const char* objectName, Action action) {
    auto* pButton = new QPushButton(text, this);
    pButton->setObjectName(objectName);
    pButton->setFocusPolicy(Qt::NoFocus);
    // Desktop/mouse path; on the touchscreen the release handler dispatches.
    connect(pButton, &QPushButton::clicked, this, action);
    m_actions.append({pButton, std::move(action)});
    return pButton;
}

void WHarnessPanel::onStateChanged() {
    updateHeader();
    rebuildSuggestions();
}

void WHarnessPanel::updateHeader() {
    HarnessBridge* pBridge = HarnessBridge::tryInstance();
    if (!pBridge) {
        m_pStatus->setText(tr("Assist is not available in this build"));
        m_pNowPlaying->clear();
        return;
    }
    QString status;
    switch (pBridge->status()) {
    case HarnessBridge::Status::Model:
        status = tr("Assistant online - picks refined by the model");
        break;
    case HarnessBridge::Status::Heuristic:
        status = tr("Assistant online - model not used");
        break;
    case HarnessBridge::Status::Offline:
        status = tr("Assistant offline");
        break;
    }
    if (!pBridge->statusDetail().isEmpty()) {
        status += QStringLiteral(": ") + pBridge->statusDetail();
    }
    m_pStatus->setText(pBridge->agentBusy() ? tr("Agent is planning…") : status);
    m_pStatus->setToolTip(status);
    m_pStatus->setProperty(kStatusProperty, statusName(pBridge->status()));
    restyle(style(), m_pStatus);

    const QString current = pBridge->currentTrackLabel();
    m_pNowPlaying->setText(current.isEmpty() ? tr("Nothing played yet") : current);
    for (QPushButton* pButton : std::as_const(m_rateButtons)) {
        pButton->setEnabled(!current.isEmpty());
        pButton->setProperty(kSelectedProperty,
                pButton->property("rating").toInt() ==
                        static_cast<int>(pBridge->currentRating()));
        restyle(style(), pButton);
    }
}

void WHarnessPanel::rebuildSuggestions() {
    // Detach now so the old rows stop painting, delete later so a click that
    // triggered this rebuild can finish being dispatched (see WUsbList).
    for (QWidget* pWidget : std::as_const(m_rowWidgets)) {
        m_actions.removeIf([pWidget](const QPair<QPushButton*, Action>& entry) {
            return entry.first == pWidget;
        });
        pWidget->setParent(nullptr);
        pWidget->deleteLater();
    }
    m_rowWidgets.clear();

    HarnessBridge* pBridge = HarnessBridge::tryInstance();
    const QList<HarnessBridge::Suggestion> suggestions =
            pBridge ? pBridge->suggestions() : QList<HarnessBridge::Suggestion>();
    if (suggestions.isEmpty()) {
        auto* pEmpty = new QLabel(pBridge && pBridge->status() != HarnessBridge::Status::Offline
                        ? tr("No suggestions yet. They appear once a drive is "
                             "scanned or a track has played.")
                        : tr("Suggestions appear when the assistant is running."),
                this);
        pEmpty->setObjectName(kEmptyObjectName);
        pEmpty->setWordWrap(true);
        m_pLayout->addWidget(pEmpty, kFirstSuggestionRow, 0, 1, kColumns);
        m_rowWidgets.append(pEmpty);
        return;
    }

    for (int i = 0; i < suggestions.size(); ++i) {
        const HarnessBridge::Suggestion& suggestion = suggestions.at(i);
        const int row = kFirstSuggestionRow + i;
        QStringList details;
        if (suggestion.bpm > 0) {
            details.append(QStringLiteral("%1 BPM").arg(suggestion.bpm, 0, 'f', 1));
        }
        if (!suggestion.key.isEmpty()) {
            details.append(suggestion.key);
        }
        if (!suggestion.reason.isEmpty()) {
            details.append(suggestion.reason);
        }
        const QString title = suggestion.artist.isEmpty()
                ? suggestion.title
                : suggestion.title + QStringLiteral(" - ") + suggestion.artist;
        auto* pLabel = new QLabel(
                title + QLatin1Char('\n') + details.join(QStringLiteral("  ·  ")), this);
        pLabel->setObjectName(kSuggestionObjectName);
        pLabel->setTextFormat(Qt::PlainText);
        pLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_pLayout->addWidget(pLabel, row, 0);
        m_rowWidgets.append(pLabel);

        for (int deck = 1; deck <= HarnessBridge::kLoadDeckCount; ++deck) {
            QPushButton* pLoad = addButton(tr("Load %1").arg(deck), kLoadButtonObjectName, [i, deck] {
                if (HarnessBridge* pBridge = HarnessBridge::tryInstance()) {
                    pBridge->loadSuggestion(i, deck);
                }
            });
            m_pLayout->addWidget(pLoad, row, deck);
            pLoad->setEnabled(!pBridge->agentBusy());
            m_rowWidgets.append(pLoad);
        }
        QPushButton* pSkip = addButton(tr("Skip"), kSkipButtonObjectName, [i] {
            if (HarnessBridge* pBridge = HarnessBridge::tryInstance()) {
                pBridge->skipSuggestion(i);
            }
        });
        m_pLayout->addWidget(pSkip, row, kColumns - 1);
        m_rowWidgets.append(pSkip);
    }
}

void WHarnessPanel::showAgentSettings() {
    auto* bridge = HarnessBridge::tryInstance();
    if (!bridge) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Agent models · OpenRouter"));
    dialog->resize(640, 420);
    auto* layout = new QVBoxLayout(dialog);
    auto* note = new QLabel(tr("A provisioned key loads automatically; otherwise enter it for this run. "
                              "Runtime changes stay in memory. Song metadata and crowd ratings are sent "
                              "to OpenRouter as advice updates; audio and paths stay local."), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    auto* key = new QLineEdit(dialog);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(tr("OpenRouter API key"));
    auto* quick = new QComboBox(dialog);
    auto* planner = new QComboBox(dialog);
    quick->setEditable(true);
    planner->setEditable(true);
    quick->setInsertPolicy(QComboBox::NoInsert);
    planner->setInsertPolicy(QComboBox::NoInsert);
    quick->setEditText(QStringLiteral("openrouter/auto"));
    planner->setEditText(QStringLiteral("openrouter/auto"));
    form->addRow(tr("API key"), key);
    form->addRow(tr("Next-song model"), quick);
    form->addRow(tr("Setlist model"), planner);
    layout->addLayout(form);
    auto* status = new QLabel(tr("Load the model catalog or enter model IDs directly."), dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto* buttons = new QHBoxLayout;
    auto* catalog = new QPushButton(tr("Load models"), dialog);
    auto* save = new QPushButton(tr("Apply"), dialog);
    auto* offline = new QPushButton(tr("Disconnect"), dialog);
    auto* close = new QPushButton(tr("Close"), dialog);
    for (auto* button : {catalog, save, offline, close}) {
        button->setMinimumHeight(44);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    bridge->agentRequest(QStringLiteral("/api/agent/settings"), {}, dialog,
            [quick, planner, key, status](const QJsonObject& reply) {
                if (reply.contains(QStringLiteral("error"))) {
                    status->setText(reply.value(QStringLiteral("error")).toString());
                    return;
                }
                if (reply.value(QStringLiteral("connected")).toBool()) {
                    quick->setEditText(reply.value(QStringLiteral("next_model")).toString());
                    planner->setEditText(reply.value(QStringLiteral("plan_model")).toString());
                    key->setPlaceholderText(QObject::tr("Leave blank to keep the current runtime key"));
                    status->setText(reply.value(QStringLiteral("provisioned")).toBool()
                                    ? QObject::tr("Provisioned at install. Runtime changes reset on restart. OpenRouter bills model use.")
                                    : QObject::tr("Configured for this run. OpenRouter bills model use."));
                } else if (!reply.value(QStringLiteral("provision_error")).toString().isEmpty()) {
                    status->setText(reply.value(QStringLiteral("provision_error")).toString());
                }
            });
    connect(catalog, &QPushButton::clicked, dialog, [=] {
        catalog->setEnabled(false);
        status->setText(tr("Loading OpenRouter models…"));
        bridge->agentRequest(QStringLiteral("/api/agent/models"), {}, dialog, [=](const QJsonObject& reply) {
            catalog->setEnabled(true);
            if (reply.contains(QStringLiteral("error"))) {
                status->setText(reply.value(QStringLiteral("error")).toString());
                return;
            }
            const QString quickText = quick->currentText();
            const QString planText = planner->currentText();
            quick->clear();
            planner->clear();
            for (const auto& item : reply.value(QStringLiteral("models")).toArray()) {
                const QString id = item.toObject().value(QStringLiteral("id")).toString();
                quick->addItem(id);
                planner->addItem(id);
            }
            quick->setEditText(quickText);
            planner->setEditText(planText);
            status->setText(tr("Choose models or type an ID. Prices vary by model."));
        });
    });
    auto apply = [=](bool disconnect) {
        QJsonObject body;
        if (disconnect) {
            body.insert(QStringLiteral("disconnect"), true);
        } else {
            body = {{QStringLiteral("api_key"), key->text()},
                    {QStringLiteral("next_model"), quick->currentText()},
                    {QStringLiteral("plan_model"), planner->currentText()}};
        }
        save->setEnabled(false);
        offline->setEnabled(false);
        key->clear();
        bridge->agentRequest(QStringLiteral("/api/agent/settings"), body, dialog, [=](const QJsonObject& reply) {
            save->setEnabled(true);
            offline->setEnabled(true);
            if (reply.contains(QStringLiteral("error"))) {
                status->setText(reply.value(QStringLiteral("error")).toString());
                return;
            }
            status->setText(disconnect ? tr("Disconnected for this run. Local scoring is active. Provisioned settings reload on restart.")
                                      : tr("Applied. Requesting advice to check the selected model…"));
            bridge->refreshSuggestions();
        });
    };
    connect(save, &QPushButton::clicked, dialog, [apply] { apply(false); });
    connect(offline, &QPushButton::clicked, dialog, [apply] { apply(true); });
    dialog->open();
}

void WHarnessPanel::showSetlist() {
    auto* bridge = HarnessBridge::tryInstance();
    if (!bridge) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Agent setlist"));
    dialog->resize(760, 480);
    auto* layout = new QVBoxLayout(dialog);
    auto* status = new QLabel(dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto* controls = new QHBoxLayout;
    auto* count = new QSpinBox(dialog);
    count->setRange(1, 100);
    count->setValue(bridge->agentPlan().value(QStringLiteral("requested")).toInt(10));
    count->setSuffix(tr(" songs"));
    controls->addWidget(count);
    for (const auto& choice : QList<QPair<QString, QString>>{
                 {tr("Follow crowd"), QStringLiteral("auto")},
                 {tr("Build"), QStringLiteral("up")},
                 {tr("Hold"), QStringLiteral("steady")},
                 {tr("Ease down"), QStringLiteral("down")}}) {
        auto* button = new QPushButton(choice.first, dialog);
        button->setMinimumHeight(44);
        controls->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, [bridge, count, direction = choice.second] {
            bridge->planSet(count->value(), direction);
        });
    }
    layout->addLayout(controls);
    auto* list = new QListWidget(dialog);
    list->setWordWrap(true);
    layout->addWidget(list);
    auto* buttons = new QHBoxLayout;
    auto* load1 = new QPushButton(tr("Load 1"), dialog);
    auto* load2 = new QPushButton(tr("Load 2"), dialog);
    auto* exportButton = new QPushButton(tr("Export M3U"), dialog);
    auto* clear = new QPushButton(tr("Clear plan"), dialog);
    auto* close = new QPushButton(tr("Close"), dialog);
    for (auto* button : {load1, load2, exportButton, clear, close}) {
        button->setMinimumHeight(44);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    auto refresh = [=] {
        const QJsonObject plan = bridge->agentPlan();
        const int selected = list->currentRow();
        list->clear();
        for (const auto& value : plan.value(QStringLiteral("tracks")).toArray()) {
            const QJsonObject track = value.toObject();
            list->addItem(QStringLiteral("%1. %2 — %3 · %4 BPM\n%5")
                                  .arg(list->count() + 1)
                                  .arg(track.value(QStringLiteral("title")).toString())
                                  .arg(track.value(QStringLiteral("artist")).toString())
                                  .arg(track.value(QStringLiteral("bpm")).toDouble(), 0, 'f', 1)
                                  .arg(track.value(QStringLiteral("model_reason")).toString()));
        }
        list->setCurrentRow(selected >= 0 && selected < list->count() ? selected : 0);
        status->setText(bridge->agentBusy() ? tr("Updating the plan…")
                : plan.isEmpty() ? tr("Choose a direction to generate a setlist. Crowd ratings adjust the upcoming songs.")
                                 : QStringLiteral("%1 / %2 songs · %3\n%4")
                                           .arg(plan.value(QStringLiteral("returned")).toInt())
                                           .arg(plan.value(QStringLiteral("requested")).toInt())
                                           .arg(plan.value(QStringLiteral("summary")).toString())
                                           .arg(plan.value(QStringLiteral("model_error")).toString()));
        for (auto* button : {load1, load2, exportButton}) {
            button->setEnabled(!bridge->agentBusy() && list->count() > 0);
        }
    };
    connect(bridge, &HarnessBridge::stateChanged, dialog, refresh);
    connect(load1, &QPushButton::clicked, dialog, [=] { bridge->loadPlanTrack(list->currentRow(), 1); });
    connect(load2, &QPushButton::clicked, dialog, [=] { bridge->loadPlanTrack(list->currentRow(), 2); });
    connect(clear, &QPushButton::clicked, dialog, [=] { bridge->planSet(count->value(), QStringLiteral("auto"), true); });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    connect(exportButton, &QPushButton::clicked, dialog, [=] {
        const QString path = QFileDialog::getSaveFileName(dialog, tr("Export setlist"),
                QStringLiteral("mrow-setlist.m3u"), tr("M3U playlists (*.m3u)"));
        if (path.isEmpty()) {
            return;
        }
        bridge->agentRequest(QStringLiteral("/api/agent/export"),
                {{QStringLiteral("basis"), bridge->agentPlan().value(QStringLiteral("basis"))}}, dialog,
                [=](const QJsonObject& reply) {
                    if (reply.contains(QStringLiteral("error"))) {
                        status->setText(reply.value(QStringLiteral("error")).toString());
                        return;
                    }
                    QSaveFile file(path);
                    const QByteArray content = reply.value(QStringLiteral("content")).toString().toUtf8();
                    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
                        status->setText(tr("Could not save the playlist."));
                        return;
                    }
                    status->setText(tr("Playlist exported."));
                });
    });
    refresh();
    dialog->open();
}

void WHarnessPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    // Dispatched on release, like a button.
    m_pressPending = true;
    e->accept();
}

void WHarnessPanel::mouseReleaseEvent(QMouseEvent* e) {
    if (!m_pressPending) {
        WWidget::mouseReleaseEvent(e);
        return;
    }
    m_pressPending = false;
    const QPoint globalPos = e->globalPosition().toPoint();
    // Copy: the action may rebuild the rows, and with them m_actions.
    const QList<QPair<QPushButton*, Action>> actions = m_actions;
    for (const auto& [pButton, action] : actions) {
        if (pButton->isVisible() && pButton->isEnabled() &&
                pButton->rect().contains(pButton->mapFromGlobal(globalPos))) {
            action();
            break;
        }
    }
    e->accept();
}
