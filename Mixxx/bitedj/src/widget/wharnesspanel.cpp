#include "widget/wharnesspanel.h"

#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>

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
    m_pLayout->addWidget(pHeader, 2, 0, 1, kColumns);

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
    m_pStatus->setText(status);
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
