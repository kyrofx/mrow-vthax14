#include "widget/wharnesspanel.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>

#include "control/controlpushbutton.h"
#include "test/mixxxtest.h"

namespace {
class WHarnessPanelTest : public MixxxTest {
  protected:
    void SetUp() override {
        touch = std::make_unique<ControlPushButton>(ConfigKey("[Controls]", "touch_shift"));
        panel = std::make_unique<WHarnessPanel>();
        panel->setStyleSheet(QStringLiteral("QPushButton { min-height: 44px; }"));
        panel->resize(800, 120);
        panel->show();
        QCoreApplication::processEvents();
        scroll = panel->findChild<QScrollArea*>();
    }
    void send(QEvent::Type type, QPoint global) {
        const QPointF local = panel->mapFromGlobal(global);
        QMouseEvent event(type, local, local, QPointF(global),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel.get(), &event);
    }
    std::unique_ptr<ControlPushButton> touch;
    std::unique_ptr<WHarnessPanel> panel;
    QScrollArea* scroll = nullptr;
};

TEST_F(WHarnessPanelTest, OverflowCanReachGenerateButton) {
    ASSERT_NE(nullptr, scroll);
    EXPECT_EQ(120, panel->height());
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    QPushButton* generate = nullptr;
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Generate song")) {
            generate = button;
        }
    }
    ASSERT_NE(nullptr, generate);
    scroll->ensureWidgetVisible(generate);
    EXPECT_TRUE(scroll->viewport()->rect().contains(
            generate->mapTo(scroll->viewport(), generate->rect().center())));
}

TEST_F(WHarnessPanelTest, TouchDragScrollsAndRefreshPreservesPosition) {
    ASSERT_NE(nullptr, scroll);
    const QPoint start = scroll->viewport()->mapToGlobal(QPoint(80, 90));
    send(QEvent::MouseButtonPress, start);
    send(QEvent::MouseMove, start - QPoint(0, 50));
    send(QEvent::MouseButtonRelease, start - QPoint(0, 50));
    EXPECT_EQ(50, scroll->verticalScrollBar()->value());
    QMetaObject::invokeMethod(panel.get(), "onStateChanged", Qt::DirectConnection);
    QCoreApplication::processEvents();
    EXPECT_EQ(50, scroll->verticalScrollBar()->value());
}

TEST_F(WHarnessPanelTest, CancelledTouchDoesNotKeepScrolling) {
    const QPoint start = scroll->viewport()->mapToGlobal(QPoint(80, 90));
    send(QEvent::MouseButtonPress, start);
    QEvent cancel(QEvent::TouchCancel);
    QCoreApplication::sendEvent(panel.get(), &cancel);
    send(QEvent::MouseMove, start - QPoint(0, 50));
    EXPECT_EQ(0, scroll->verticalScrollBar()->value());
}
} // namespace
