#include "skin/legacy/launchimage.h"

#include <QEvent>
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
constexpr int kBlackFrameMs = 80;
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
    m_pContent = content;
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

}

void LaunchImage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_started) {
        return;
    }
    m_started = true;
    // Keep the artwork static while startup occupies the GUI thread.
    QTimer::singleShot(kMinimumVisibleMs, this, [this] {
        m_minimumElapsed = true;
        tryFadeOut();
    });
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
    // Let the current initialization call return before starting the transition.
    QTimer::singleShot(0, this, &LaunchImage::tryFadeOut);
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
    // Capture once, then paint only that image over opaque black. Avoid widget
    // graphics effects and blending against the live OpenGL DJ interface.
    m_fadeFrame = grab();
    m_pContent->hide();
    m_fadingOut = true;
    auto* fade = new QPropertyAnimation(this, "fadeOpacity", this);
    fade->setDuration(kFadeDurationMs);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, [this] {
        repaint();
        // Give the compositor a fully black frame before revealing the skin.
        QTimer::singleShot(kBlackFrameMs, this, &QObject::deleteLater);
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LaunchImage::progress(int value, const QString& serviceName) {
    m_pProgressBar->setValue(value);
    // TODO: show serviceName
    Q_UNUSED(serviceName);
}

void LaunchImage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_fadingOut) {
        p.setOpacity(m_fadeOpacity);
        p.drawPixmap(rect(), m_fadeFrame);
        return;
    }
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
