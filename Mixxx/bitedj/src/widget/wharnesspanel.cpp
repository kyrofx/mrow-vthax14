#include "widget/wharnesspanel.h"

#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <cmath>
#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
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
constexpr int kFirstSuggestionRow = 4;

void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}

void presentDialog(QDialog* dialog) {
    dialog->setAttribute(Qt::WA_StyledBackground);
    dialog->setWindowModality(Qt::ApplicationModal);
    if (!dialog->findChild<QLabel*>(QStringLiteral("AssistDialogTitle"))) {
        auto* title = new QLabel(dialog->windowTitle(), dialog);
        title->setObjectName(QStringLiteral("AssistDialogTitle"));
        title->setWordWrap(true);
        title->setTextFormat(Qt::PlainText);
        if (auto* layout = qobject_cast<QVBoxLayout*>(dialog->layout())) {
            layout->insertWidget(0, title);
        }
    }
    dialog->open();
    dialog->raise();
    dialog->activateWindow();
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
          m_pScrollArea(new QScrollArea(this)),
          m_pContent(new QWidget(m_pScrollArea)),
          m_pLayout(new QGridLayout(m_pContent)),
          m_pStatus(new QLabel(this)),
          m_pResponse(new QLabel(this)),
          m_pNowPlaying(new QLabel(this)) {
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(true); // WWidget's synthetic touch moves have no held button.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_pResponse->setObjectName(QStringLiteral("HarnessResponse"));
    m_pResponse->setTextFormat(Qt::PlainText);
    m_pResponse->setWordWrap(true);
    m_pResponse->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pResponse->hide();
    outer->addWidget(m_pResponse);
    outer->addWidget(m_pScrollArea);
    m_pScrollArea->setObjectName(QStringLiteral("HarnessScrollArea"));
    m_pContent->setObjectName(QStringLiteral("HarnessScrollContent"));
    m_pScrollArea->setFrameShape(QFrame::NoFrame);
    m_pScrollArea->setWidgetResizable(true);
    m_pScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScrollArea->viewport()->setAutoFillBackground(false);
    m_pScrollArea->verticalScrollBar()->setSingleStep(40);
    m_pLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_pScrollArea->setWidget(m_pContent);
    // setWidget() enables auto-fill; disable it after attaching the content.
    m_pContent->setAutoFillBackground(false);

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

    auto* pHeader = new QLabel(tr("Up next · up to 12"), this);
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

    m_pLayout->addWidget(addButton(tr("Generate song"), kRefreshButtonObjectName,
                                [this] { showMusicGeneration(); }), 3, 0, 1, kColumns);

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

WHarnessPanel* WHarnessPanel::activePanel() {
    for (auto* widget : QApplication::allWidgets()) {
        if (auto* panel = qobject_cast<WHarnessPanel*>(widget); panel && panel->isVisible()) {
            return panel;
        }
    }
    return nullptr;
}

void WHarnessPanel::moveSelection(int steps) {
    auto* bridge = HarnessBridge::tryInstance();
    if (!bridge || QApplication::activeModalWidget()) {
        return;
    }
    const auto suggestions = bridge->suggestions();
    if (suggestions.isEmpty()) {
        return;
    }
    int selected = 0;
    for (int i = 0; i < suggestions.size(); ++i) {
        if (suggestions.at(i).trackId == m_selectedTrackId) selected = i;
    }
    const int count = suggestions.size();
    selected = ((selected + steps % count) % count + count) % count;
    m_selectedTrackId = suggestions.at(selected).trackId;
    updateSelection(true);
}

bool WHarnessPanel::loadSelectedTrack(const QString& group) {
    auto* bridge = HarnessBridge::tryInstance();
    if (!bridge || QApplication::activeModalWidget() || !isVisible()) {
        return false;
    }
    const int deck = group == QStringLiteral("[Channel1]") ? 1
            : group == QStringLiteral("[Channel2]") ? 2 : 0;
    if (!deck) {
        return false;
    }
    const auto suggestions = bridge->suggestions();
    for (int i = 0; i < suggestions.size(); ++i) {
        if (suggestions.at(i).trackId == m_selectedTrackId) {
            return bridge->loadSuggestion(i, deck);
        }
    }
    return false;
}

void WHarnessPanel::updateSelection(bool reveal) {
    for (auto* row : m_suggestionRows) {
        const bool selected = row->property("suggestionTrackId").toString() == m_selectedTrackId;
        row->setProperty(kSelectedProperty, selected);
        restyle(row->style(), row);
        row->update();
    }
    if (reveal) {
        // A fresh response can replace rows in the same event turn as a knob
        // tick. Let the scroll area's layout/range catch up before revealing.
        QTimer::singleShot(0, this, [this] {
            for (auto* row : m_suggestionRows) {
                if (row->property("suggestionTrackId").toString() == m_selectedTrackId) {
                    m_pScrollArea->ensureWidgetVisible(row, 0, 12);
                    break;
                }
            }
        });
    }
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
    const QString response = pBridge->adviceSummary();
    m_pResponse->setVisible(!response.isEmpty());
    m_pResponse->setText((pBridge->agentBusy() ? tr("Updating advice…\n") : tr("Agent response\n")) + response);
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
    m_suggestionRows.clear();

    HarnessBridge* pBridge = HarnessBridge::tryInstance();
    const QList<HarnessBridge::Suggestion> suggestions =
            pBridge ? pBridge->suggestions() : QList<HarnessBridge::Suggestion>();
    if (suggestions.isEmpty()) {
        m_selectedTrackId.clear();
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

    bool selectionPresent = false;
    for (const auto& suggestion : suggestions) {
        selectionPresent |= suggestion.trackId == m_selectedTrackId;
    }
    if (!selectionPresent) {
        m_selectedTrackId = suggestions.first().trackId;
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
        const QString title = suggestion.artist == QStringLiteral("ElevenLabs")
                ? QStringLiteral("EL: ") + suggestion.title
                : suggestion.artist.isEmpty()
                ? suggestion.title
                : suggestion.title + QStringLiteral(" - ") + suggestion.artist;
        auto* pLabel = new QLabel(
                title + QLatin1Char('\n') + details.join(QStringLiteral("  ·  ")), this);
        pLabel->setObjectName(kSuggestionObjectName);
        pLabel->setProperty("suggestionTrackId", suggestion.trackId);
        m_suggestionRows.append(pLabel);
        pLabel->setTextFormat(Qt::PlainText);
        pLabel->setWordWrap(true);
        pLabel->setMinimumHeight(72);
        pLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
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
    updateSelection(false);
}

QDialog* WHarnessPanel::createAssistDialog(const QString& name, const QString& title, const QSize& size) {
    if (auto* existing = findChild<QDialog*>(name)) {
        presentDialog(existing);
        return nullptr;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(name);
    dialog->setProperty("bitedjAssistDialog", true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    dialog->resize(size);
    auto* layout = new QVBoxLayout(dialog);
    if (!HarnessBridge::tryInstance()) {
        auto* error = new QLabel(tr("The assistant is unavailable. Restart BiteDJ to start the built-in agent."), dialog);
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* close = new QPushButton(tr("Close"), dialog);
        close->setMinimumHeight(44);
        layout->addWidget(close);
        connect(close, &QPushButton::clicked, dialog, &QDialog::close);
        presentDialog(dialog);
        return nullptr;
    }
    return dialog;
}

void WHarnessPanel::showAgentSettings() {
    auto* dialog = createAssistDialog(QStringLiteral("HarnessModelsDialog"), tr("Agent models · Google Cloud Gemini"), QSize(640, 420));
    if (!dialog) {
        return;
    }
    auto* bridge = HarnessBridge::tryInstance();
    auto* layout = qobject_cast<QVBoxLayout*>(dialog->layout());
    auto* note = new QLabel(tr("Keys load from the installed configuration. Song metadata and ratings are sent to the configured model; audio stays local."), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    auto* quick = new QComboBox(dialog);
    auto* planner = new QComboBox(dialog);
    quick->setEditable(true);
    planner->setEditable(true);
    quick->setInsertPolicy(QComboBox::NoInsert);
    planner->setInsertPolicy(QComboBox::NoInsert);
    quick->setEditText(QStringLiteral("gemini-2.5-flash"));
    planner->setEditText(QStringLiteral("gemini-2.5-flash"));
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
    auto* close = new QPushButton(tr("Close"), dialog);
    for (auto* button : {catalog, save, close}) {
        button->setMinimumHeight(44);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    bridge->agentRequest(QStringLiteral("/api/agent/settings"), {}, dialog,
            [quick, planner, status](const QJsonObject& reply) {
                if (reply.contains(QStringLiteral("error"))) {
                    status->setText(reply.value(QStringLiteral("error")).toString());
                    return;
                }
                if (reply.value(QStringLiteral("connected")).toBool()) {
                    quick->setEditText(reply.value(QStringLiteral("next_model")).toString());
                    planner->setEditText(reply.value(QStringLiteral("plan_model")).toString());
                    status->setText(reply.value(QStringLiteral("provisioned")).toBool()
                                    ? QObject::tr("API key configured at install. Model changes reset on restart.")
                                    : QObject::tr("API key configured for this run."));
                } else if (!reply.value(QStringLiteral("provision_error")).toString().isEmpty()) {
                    status->setText(reply.value(QStringLiteral("provision_error")).toString());
                } else {
                    status->setText(tr("API key not configured. Provision it during installation and restart."));
                }
            });
    connect(catalog, &QPushButton::clicked, dialog, [=] {
        catalog->setEnabled(false);
        status->setText(tr("Loading Gemini suggestions…"));
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
            body = {{QStringLiteral("next_model"), quick->currentText()},
                    {QStringLiteral("plan_model"), planner->currentText()}};
        }
        save->setEnabled(false);
        bridge->agentRequest(QStringLiteral("/api/agent/settings"), body, dialog, [=](const QJsonObject& reply) {
            save->setEnabled(true);
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
    presentDialog(dialog);
}

void WHarnessPanel::showSetlist() {
    auto* dialog = createAssistDialog(QStringLiteral("HarnessSetlistDialog"), tr("Agent setlist"), QSize(760, 480));
    if (!dialog) {
        return;
    }
    auto* bridge = HarnessBridge::tryInstance();
    auto* layout = qobject_cast<QVBoxLayout*>(dialog->layout());
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
    presentDialog(dialog);
}

void WHarnessPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }

    // Map via global coords so the hit-test is correct regardless of which
    // widget the synthesized event's local position referenced, and regardless
    // of how far the content is scrolled.
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    if (pScrollBar->isVisible() &&
            pScrollBar->rect().contains(
                    pScrollBar->mapFromGlobal(e->globalPosition().toPoint()))) {
        m_dragState = DragState::ScrollBar;
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    // Hold the press back until we know whether this is a tap or a drag; the
    // button action is dispatched on release.
    m_dragState = DragState::Pending;
    m_pressGlobalPos = e->globalPosition();
    m_lastGlobalY = m_pressGlobalPos.y();
    m_remainingDy = 0;
    e->accept();
}

void WHarnessPanel::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragState == DragState::Idle) {
        WWidget::mouseMoveEvent(e);
        return;
    }
    if (m_dragState == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    const qreal globalY = e->globalPosition().y();
    if (m_dragState == DragState::Pending) {
        if (std::abs(globalY - m_pressGlobalPos.y()) < QApplication::startDragDistance()) {
            // Might still become a tap, keep swallowing.
            e->accept();
            return;
        }
        m_dragState = DragState::Scrolling;
        // m_lastGlobalY is still the press position, so the content catches up
        // with the finger in this first step and stays pinned to it.
    }

    // Scroll bar values are integers, carry the remainder over to the next
    // move so slow drags don't get lost in rounding.
    m_remainingDy += m_lastGlobalY - globalY;
    const int scrollBy = static_cast<int>(m_remainingDy);
    if (scrollBy != 0) {
        m_remainingDy -= scrollBy;
        QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
        pScrollBar->setValue(pScrollBar->value() + scrollBy);
    }
    m_lastGlobalY = globalY;
    e->accept();
}

bool WHarnessPanel::event(QEvent* e) {
    if (e->type() == QEvent::TouchCancel || e->type() == QEvent::Hide) {
        m_dragState = DragState::Idle;
    }
    return WWidget::event(e);
}

void WHarnessPanel::mouseReleaseEvent(QMouseEvent* e) {
    const DragState state = m_dragState;
    m_dragState = DragState::Idle;
    if (state == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    const QPoint globalPos = e->globalPosition().toPoint();
    const auto* viewport = m_pScrollArea->viewport();
    if (state == DragState::Pending &&
            (e->globalPosition() - m_pressGlobalPos).manhattanLength() < QApplication::startDragDistance() &&
            viewport->rect().contains(viewport->mapFromGlobal(globalPos))) {
        // Only a tap beginning and ending on the same visible button activates it.
        const auto actions = m_actions;
        for (const auto& [button, action] : actions) {
            if (button->isVisible() && button->isEnabled() &&
                    button->rect().contains(button->mapFromGlobal(m_pressGlobalPos.toPoint())) &&
                    button->rect().contains(button->mapFromGlobal(globalPos))) {
                action();
                break;
            }
        }
    }
    e->accept();
}

void WHarnessPanel::forwardToScrollBar(QMouseEvent* pEvent) {
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    const QPointF localPos = pScrollBar->mapFromGlobal(pEvent->globalPosition().toPoint());
    const bool isRelease = pEvent->type() == QEvent::MouseButtonRelease;
    const bool isMove = pEvent->type() == QEvent::MouseMove;
    const QPointingDevice* pDevice = pEvent->pointingDevice()
            ? pEvent->pointingDevice()
            : QPointingDevice::primaryPointingDevice();
    QMouseEvent forwarded(pEvent->type(),
            localPos,
            localPos,
            pEvent->globalPosition(),
            isMove ? Qt::NoButton : Qt::LeftButton,
            isRelease ? Qt::NoButton : Qt::LeftButton,
            pEvent->modifiers(),
            pDevice);
    QCoreApplication::sendEvent(pScrollBar, &forwarded);
}

void WHarnessPanel::showMusicGeneration() {
    if (auto* existing = findChild<QDialog*>(QStringLiteral("HarnessMusicDialog"))) {
        presentDialog(existing);
        return;
    }
    auto* bridge = HarnessBridge::tryInstance();
    auto* dialog = new QDialog(this);
    dialog->setProperty("bitedjAssistDialog", true);
    dialog->setObjectName(QStringLiteral("HarnessMusicDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Generate song · ElevenLabs"));
    dialog->resize(640, 480);
    auto* layout = new QVBoxLayout(dialog);
    if (!bridge) {
        auto* error = new QLabel(tr("The assistant is unavailable. Restart BiteDJ to start the built-in agent."), dialog);
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* close = new QPushButton(tr("Close"), dialog);
        layout->addWidget(close);
        connect(close, &QPushButton::clicked, dialog, &QDialog::close);
        presentDialog(dialog);
        return;
    }
    auto* note = new QLabel(tr("Create original music from Good / Mid / Bad ratings of played songs across sets. "
                              "Each generation uses your ElevenLabs balance and saves an MP3 automatically. "
                              "Finished songs enter the library and appear first in Assist."), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    auto* duration = new QSpinBox(dialog);
    duration->setRange(3, 600);
    duration->setValue(120);
    duration->setSuffix(tr(" seconds"));
    auto* direction = new QComboBox(dialog);
    direction->addItem(tr("Follow crowd"), QStringLiteral("follow crowd"));
    direction->addItem(tr("Build"), QStringLiteral("build"));
    direction->addItem(tr("Hold"), QStringLiteral("hold"));
    direction->addItem(tr("Ease down"), QStringLiteral("ease down"));
    auto* instrumental = new QCheckBox(tr("Instrumental"), dialog);
    instrumental->setChecked(true);
    form->addRow(tr("Length"), duration);
    form->addRow(tr("Direction"), direction);
    form->addRow(instrumental);
    auto* inspire = new QCheckBox(tr("Use current song as inspiration (genre, tempo and key)"), dialog);
    inspire->setObjectName(QStringLiteral("InspireCurrentSong"));
    form->addRow(inspire);
    layout->addLayout(form);
    auto* credential = new QLabel(tr("ElevenLabs key: checking…"), dialog);
    credential->setObjectName(QStringLiteral("MusicCredentialStatus"));
    layout->addWidget(credential);
    auto* status = new QLabel(tr("Checking music settings…"), dialog);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* brief = new QLabel(tr("What we’re generating: choose your settings and generate a song."), dialog);
    brief->setObjectName(QStringLiteral("MusicCompositionBrief"));
    brief->setWordWrap(true);
    brief->setTextFormat(Qt::PlainText);
    layout->addWidget(brief);
    auto* history = new QListWidget(dialog);
    history->setWordWrap(true);
    layout->addWidget(history);
    auto* buttons = new QHBoxLayout;
    auto* generate = new QPushButton(tr("Generate & download"), dialog);
    auto* folder = new QPushButton(tr("Open downloads"), dialog);
    auto* close = new QPushButton(tr("Close"), dialog);
    for (auto* button : {generate, folder, close}) {
        button->setMinimumHeight(44);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    folder->setEnabled(false);
    generate->setEnabled(false);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    connect(folder, &QPushButton::clicked, dialog, [folder] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder->property("directory").toString()));
    });
    auto* timer = new QTimer(dialog);
    timer->setInterval(2000);
    auto refresh = [=] {
        timer->stop();
        bridge->agentRequest(QStringLiteral("/api/agent/music/view"), {}, dialog, [=](const QJsonObject& reply) {
            if (reply.contains(QStringLiteral("error"))) {
                status->setText(reply.value(QStringLiteral("error")).toString());
                timer->start();
                return;
            }
            credential->setText(reply.value(QStringLiteral("connected")).toBool()
                            ? tr("ElevenLabs key: configured") : tr("ElevenLabs key: not configured"));
            const QString directory = reply.value(QStringLiteral("directory")).toString();
            folder->setProperty("directory", directory);
            const auto jobs = reply.value(QStringLiteral("jobs")).toArray();
            if (!jobs.isEmpty()) {
                const auto latest = jobs.first().toObject();
                const bool briefing = latest.value(QStringLiteral("state")).toString() == QStringLiteral("generating") &&
                        latest.value(QStringLiteral("phase")).toString() == QStringLiteral("briefing");
                brief->setText(briefing ? tr("What we’re generating: Gemini is preparing the composition brief…")
                                : tr("What we’re generating (%1): %2\n%3")
                                          .arg(latest.value(QStringLiteral("brief_source")).toString() == QStringLiteral("gemini")
                                                          ? tr("Gemini") : tr("direct feedback"))
                                          .arg(latest.value(QStringLiteral("summary")).toString())
                                          .arg(latest.value(QStringLiteral("brief_error")).toString()));
                brief->setToolTip(latest.value(QStringLiteral("reasoning")).toString());
            }
            history->clear();
            bool pending = false;
            bool downloaded = false;
            for (const auto& value : reply.value(QStringLiteral("jobs")).toArray()) {
                const QJsonObject job = value.toObject();
                const QString state = job.value(QStringLiteral("state")).toString();
                pending |= state == QStringLiteral("generating");
                downloaded |= state == QStringLiteral("complete");
                history->addItem(job.value(QStringLiteral("created")).toString() + QStringLiteral(" · ") + state +
                        QStringLiteral(" · ") + (state == QStringLiteral("complete")
                                        ? job.value(QStringLiteral("path")).toString()
                                        : job.value(QStringLiteral("error")).toString()) +
                        QStringLiteral("\n") + job.value(QStringLiteral("summary")).toString() +
                        QStringLiteral("\n") + job.value(QStringLiteral("reasoning")).toString());
            }
            generate->setEnabled(!pending && reply.value(QStringLiteral("connected")).toBool());
            folder->setEnabled(downloaded);
            if (!status->property("keepError").toBool()) {
                status->setText(!reply.value(QStringLiteral("provision_error")).toString().isEmpty()
                                ? reply.value(QStringLiteral("provision_error")).toString()
                                : pending ? tr("Generating and downloading… You can close this window; playback remains available.")
                                : reply.value(QStringLiteral("connected")).toBool()
                                ? (reply.value(QStringLiteral("provisioned")).toBool()
                                                ? tr("Key configured. Provisioned keys reload on restart. Downloads: %1")
                                                : tr("Key configured for this run. Downloads: %1")).arg(directory)
                                : tr("ElevenLabs key not configured. Provision it during installation and restart."));
            }
            timer->start();
        });
    };
    connect(timer, &QTimer::timeout, dialog, refresh);
    connect(generate, &QPushButton::clicked, dialog, [=] {
        timer->stop();
        generate->setEnabled(false);
        status->setProperty("keepError", false);
        const QJsonObject request{{QStringLiteral("duration_seconds"), duration->value()},
                {QStringLiteral("direction"), direction->currentData().toString()},
                {QStringLiteral("instrumental"), instrumental->isChecked()},
                {QStringLiteral("inspire_current"), inspire->isChecked()}};
        status->setText(tr("Starting generation…"));
        bridge->agentRequest(QStringLiteral("/api/agent/music/generate"), request, dialog, [=](const QJsonObject& result) {
            if (result.contains(QStringLiteral("error"))) {
                status->setProperty("keepError", true);
                status->setText(result.value(QStringLiteral("error")).toString());
            }
            refresh();
        });
    });
    refresh();
    presentDialog(dialog);
}
