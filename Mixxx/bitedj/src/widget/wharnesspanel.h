#pragma once

#include <QList>
#include <QPair>
#include <functional>

#include "widget/wwidget.h"

class QDomNode;
class QGridLayout;
class QLabel;
class QPushButton;
class SkinContext;

// Bite DJ: the Assist tab. Shows what HarnessBridge knows: the play being
// rated with Good / Mid / Bad, the harness's next-song suggestions (each with
// its reason, Load 1 / Load 2 and Skip), and whether the assistant and its
// cloud model are reachable. Rebuilds on HarnessBridge::stateChanged; stays
// empty when the bridge was never constructed (stock Mixxx fallback).
//
// Suggestions are only ever loaded on a tap: nothing here loads a track by
// itself, and the bridge refuses to load into a playing deck.
//
// Touch handling follows WUsbList: WWidget turns touches into mouse events on
// this widget rather than on the child buttons, so taps are hit-tested here on
// release; a real mouse click (desktop, VNC) reaches the button directly.
class WHarnessPanel : public WWidget {
    Q_OBJECT
  public:
    explicit WHarnessPanel(QWidget* parent = nullptr);

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

  private slots:
    void onStateChanged();

  private:
    using Action = std::function<void()>;

    QPushButton* addButton(const QString& text, const char* objectName, Action action);
    void rebuildSuggestions();
    void updateHeader();
    void showAgentSettings();
    void showSetlist();
    void showMusicGeneration();

    QGridLayout* m_pLayout;
    QLabel* m_pStatus;
    QLabel* m_pNowPlaying;
    QList<QPushButton*> m_rateButtons;
    // Suggestion rows, rebuilt on every state change.
    QList<QWidget*> m_rowWidgets;
    // Every tappable button with what it does, for the touch hit-test.
    QList<QPair<QPushButton*, Action>> m_actions;
    bool m_pressPending;
};
