#pragma once

#include <QPixmap>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QProgressBar);
QT_FORWARD_DECLARE_CLASS(QLabel);

// This is a widget that is shown in the Mixxx main window
// until the skin is ready to use.
// It shows a centered Image and a progress bar below.
// By default a symbolic Mixxx icon and logo is shown.
// It can be modified in the skin.xml file at <skin> section,
// to match the skin like that:
//    <LaunchImageStyle>
//        LaunchImage { background-color: #202020; }
//        QLabel {
//            image: url(skin:/style/mixxx-icon-logo-symbolic.svg);
//            padding:0;
//            margin:0;
//            border:none;
//            min-width: 208px;
//            min-height: 48px;
//            max-width: 208px;
//            max-height: 48px;
//        }
//        QProgressBar {
//            background-color: #202020;
//            border:none;
//            min-width: 208px;
//            min-height: 3px;
//            max-width: 208px;
//            max-height: 3px;
//        }
//        QProgressBar::chunk { background-color: #ec4522; }
//    </LaunchImageStyle>

class LaunchImage: public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal fadeOpacity READ fadeOpacity WRITE setFadeOpacity)
  public:
    LaunchImage(QWidget* pParent, const QString& styleSheet);
    ~LaunchImage() override = default;
    void progress(int value, const QString& serviceName);
    // Keep covering the loaded skin until the minimum display time has elapsed.
    void finishWhenReady();

  protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QProgressBar* m_pProgressBar;
    QWidget* m_pContent;
    QPixmap m_fadeFrame;
    qreal m_fadeOpacity = 1.0;
    bool m_started = false;
    bool m_minimumElapsed = false;
    bool m_ready = false;
    bool m_fadingOut = false;
    void tryFadeOut();
    qreal fadeOpacity() const { return m_fadeOpacity; }
    void setFadeOpacity(qreal opacity) {
        m_fadeOpacity = opacity;
        update();
    }
};
