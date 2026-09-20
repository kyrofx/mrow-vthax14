#include "skin/legacy/launchimage.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>

#include "moc_launchimage.cpp"

namespace {
constexpr int kFadeDurationMs = 400;
constexpr int kMinimumVisibleMs = 3000;
} // namespace

LaunchImage::LaunchImage(QWidget* pParent, const QString& styleSheet)
        : QWidget(pParent) {
    if (styleSheet.isEmpty()) {
        setStyleSheet(
                "LaunchImage { background-color: #202020; }"
                "QLabel { "
                "image: url(:/images/mixxx-icon-logo-symbolic.svg);"
                "padding:0;"
                "margin:0;"
                "border:none;"
                "min-width: 236px;"
                "min-height: 48px;"
                "max-width: 236px;"
                "max-height: 48px;"
                "}"
                "QProgressBar {"
                "background-color: #202020; "
                "border:none;"
                "min-width: 236px;"
                "min-height: 3px;"
                "max-width: 236px;"
                "max-height: 3px;"
                "}"
                "QProgressBar::chunk { background-color: #f3efed; }");
    } else {
        setStyleSheet(styleSheet);
    }

    auto* content = new QWidget(this);
    QLabel* label = new QLabel(content);

    m_pProgressBar = new QProgressBar(content);
    m_pProgressBar->setTextVisible(false);

    QHBoxLayout* hbox = new QHBoxLayout(this);
    QVBoxLayout* vbox = new QVBoxLayout(content);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->addStretch();
    vbox->addWidget(label);
    vbox->addWidget(m_pProgressBar);
    vbox->addStretch();
    hbox->addStretch();
    hbox->addWidget(content);
    hbox->addStretch();

    // Fade the artwork in over the solid, skin-defined background.
    m_pContentOpacity = new QGraphicsOpacityEffect(content);
    m_pContentOpacity->setOpacity(0.0);
    content->setGraphicsEffect(m_pContentOpacity);
}

void LaunchImage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_started) {
        return;
    }
    m_started = true;
    auto* fade = new QPropertyAnimation(m_pContentOpacity, "opacity", this);
    fade->setDuration(kFadeDurationMs);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    connect(fade, &QPropertyAnimation::finished, this, [this] {
        // Start the hold after the fade actually finishes: slow synchronous
        // startup work must not consume the entire fully-visible interval.
        QTimer::singleShot(kMinimumVisibleMs, this, [this] {
            m_minimumElapsed = true;
            tryFadeOut();
        });
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LaunchImage::finishWhenReady() {
    if (m_ready) {
        return;
    }
    m_ready = true;
    parentWidget()->installEventFilter(this);
    setGeometry(parentWidget()->rect());
    show();
    raise();
    tryFadeOut();
}

bool LaunchImage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void LaunchImage::tryFadeOut() {
    if (!m_ready || !m_minimumElapsed || m_fadingOut) {
        return;
    }
    m_fadingOut = true;
    // Remove the content effect before applying an effect to its ancestor.
    m_pContentOpacity->setEnabled(false);
    auto* opacity = new QGraphicsOpacityEffect(this);
    opacity->setOpacity(1.0);
    setGraphicsEffect(opacity);
    auto* fade = new QPropertyAnimation(opacity, "opacity", this);
    fade->setDuration(kFadeDurationMs);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, &QObject::deleteLater);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LaunchImage::progress(int value, const QString& serviceName) {
    m_pProgressBar->setValue(value);
    // TODO: show serviceName
    Q_UNUSED(serviceName);
}

void LaunchImage::paintEvent(QPaintEvent *)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
