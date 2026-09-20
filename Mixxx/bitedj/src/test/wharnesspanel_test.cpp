#include "widget/wharnesspanel.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDialog>
#include <QCheckBox>
#include <QLineEdit>
#include <QFile>
#include <QLabel>
#include <QTest>
#include <QSpinBox>
#include "harness/harnessbridge.h"
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
TEST_F(WHarnessPanelTest, ScrollContentDoesNotPaintLightSystemBackground) {
    ASSERT_NE(nullptr, scroll);
    EXPECT_FALSE(scroll->widget()->autoFillBackground());
    EXPECT_FALSE(scroll->viewport()->autoFillBackground());
}

TEST_F(WHarnessPanelTest, GenerateClickAlwaysOpensVisibleModalDialog) {
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() != QStringLiteral("Generate song")) {
            continue;
        }
        button->click();
        QCoreApplication::processEvents();
        auto* dialog = panel->findChild<QDialog*>(QStringLiteral("HarnessMusicDialog"));
        ASSERT_NE(nullptr, dialog);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->isModal());
        // No bridge exists in this fixture: show an actionable error, not silence.
        EXPECT_TRUE(dialog->findChild<QLabel*>()->text().contains(QStringLiteral("unavailable")));
        button->click();
        QCoreApplication::processEvents();
        EXPECT_EQ(1, panel->findChildren<QDialog*>().size());
        return;
    }
    FAIL() << "Generate song button missing";
}

TEST_F(WHarnessPanelTest, ScrolledTouchTapOpensGenerateDialogButDragDoesNot) {
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() != QStringLiteral("Generate song")) {
            continue;
        }
        scroll->ensureWidgetVisible(button);
        QPoint start = button->mapToGlobal(button->rect().center());
        send(QEvent::MouseButtonPress, start);
        send(QEvent::MouseMove, start - QPoint(0, 40));
        send(QEvent::MouseButtonRelease, start - QPoint(0, 40));
        EXPECT_EQ(nullptr, panel->findChild<QDialog*>());
        scroll->ensureWidgetVisible(button);
        start = button->mapToGlobal(button->rect().center());
        send(QEvent::MouseButtonPress, start);
        send(QEvent::MouseButtonRelease, start);
        QCoreApplication::processEvents();
        auto* dialog = panel->findChild<QDialog*>();
        ASSERT_NE(nullptr, dialog);
        EXPECT_TRUE(dialog->isVisible());
        return;
    }
    FAIL() << "Generate song button missing";
}

TEST_F(WHarnessPanelTest, GenerateWithBridgeShowsMusicControls) {
    config()->setValue(ConfigKey(QStringLiteral("[Harness]"), QStringLiteral("enabled")), false);
    HarnessBridge bridge(config(), nullptr, nullptr);
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Generate song")) {
            button->click();
            QTest::qWait(20);
            auto* dialog = panel->findChild<QDialog*>();
            ASSERT_NE(nullptr, dialog);
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->isModal());
            ASSERT_NE(nullptr, dialog->findChild<QSpinBox*>());
            EXPECT_EQ(120, dialog->findChild<QSpinBox*>()->value());
            ASSERT_NE(nullptr, dialog->findChild<QCheckBox*>(QStringLiteral("InspireCurrentSong")));
            ASSERT_NE(nullptr, dialog->findChild<QLabel*>(QStringLiteral("MusicCredentialStatus")));
            ASSERT_NE(nullptr, dialog->findChild<QLabel*>(QStringLiteral("MusicCompositionBrief")));
            for (auto* input : dialog->findChildren<QLineEdit*>()) {
                EXPECT_NE(QLineEdit::Password, input->echoMode());
            }
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return;
        }
    }
    FAIL() << "Generate song button missing";
}

TEST_F(WHarnessPanelTest, ActualTouchOnScrolledChildOpensDialog) {
    auto* device = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Generate song")) {
            scroll->ensureWidgetVisible(button);
            QCoreApplication::processEvents();
            const QPoint center = button->rect().center();
            QTest::touchEvent(button, device).press(0, center, button);
            QTest::touchEvent(button, device).release(0, center, button);
            QTest::qWait(20);
            auto* dialog = panel->findChild<QDialog*>();
            ASSERT_NE(nullptr, dialog);
            EXPECT_TRUE(dialog->isVisible());
            return;
        }
    }
    FAIL() << "Generate song button missing";
}

TEST_F(WHarnessPanelTest, KioskStillDismissesUnmarkedDialogs) {
    QDialog unrelated(panel.get());
    unrelated.open();
    QTest::qWait(20);
    EXPECT_FALSE(unrelated.isVisible());
}

TEST_F(WHarnessPanelTest, BiteDjSkinRendersDarkScrollBackground) {
    QFile skin(getTestDir().filePath(QStringLiteral("../../res/skins/BiteDJ/style.qss")));
    ASSERT_TRUE(skin.open(QIODevice::ReadOnly));
    QWidget host;
    host.setObjectName(QStringLiteral("Assist"));
    host.setAttribute(Qt::WA_StyledBackground);
    host.setStyleSheet(QString::fromUtf8(skin.readAll()));
    host.resize(800, 600);
    panel->setParent(&host);
    panel->setObjectName(QStringLiteral("HarnessPanel"));
    panel->setStyleSheet(QString());
    panel->resize(800, 600);
    panel->show();
    host.show();
    QTest::qWait(20);
    // Grab the composed page: the viewport itself is intentionally transparent.
    const QImage rendered = host.grab().toImage();
    const QPoint sample = scroll->viewport()->mapTo(
            &host, QPoint(20, scroll->viewport()->height() - 20));
    EXPECT_EQ(QColor(Qt::black), rendered.pixelColor(sample));
    panel->setParent(nullptr);
}

TEST_F(WHarnessPanelTest, SetlistAndModelsTouchAlwaysPresentNamedDialogs) {
    auto* device = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    for (const auto& entry : QList<QPair<QString, QString>>{
                 {QStringLiteral("Setlist"), QStringLiteral("HarnessSetlistDialog")},
                 {QStringLiteral("Models"), QStringLiteral("HarnessModelsDialog")}}) {
        QPushButton* trigger = nullptr;
        for (auto* button : panel->findChildren<QPushButton*>()) {
            if (button->text() == entry.first) trigger = button;
        }
        ASSERT_NE(nullptr, trigger);
        scroll->ensureWidgetVisible(trigger);
        QCoreApplication::processEvents();
        const QPoint center = trigger->rect().center();
        QTest::touchEvent(trigger, device).press(0, center, trigger);
        QTest::touchEvent(trigger, device).release(0, center, trigger);
        QTest::qWait(20);
        auto* dialog = panel->findChild<QDialog*>(entry.second);
        ASSERT_NE(nullptr, dialog);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->isModal());
        ASSERT_NE(nullptr, dialog->findChild<QLabel*>(QStringLiteral("AssistDialogTitle")));
        trigger->click();
        EXPECT_EQ(1, panel->findChildren<QDialog*>(entry.second).size());
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

TEST_F(WHarnessPanelTest, SetlistAndModelsHaveVisibleControlsAndDarkBackground) {
    config()->setValue(ConfigKey("[Harness]", "enabled"), false);
    HarnessBridge bridge(config(), nullptr, nullptr);
    QFile skin(getTestDir().filePath(QStringLiteral("../../res/skins/BiteDJ/style.qss")));
    ASSERT_TRUE(skin.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(skin.readAll()));
    for (const auto& entry : QList<QPair<QString, QString>>{
                 {QStringLiteral("Setlist"), QStringLiteral("HarnessSetlistDialog")},
                 {QStringLiteral("Models"), QStringLiteral("HarnessModelsDialog")}}) {
        for (auto* button : panel->findChildren<QPushButton*>()) {
            if (button->text() != entry.first) continue;
            button->click();
            QTest::qWait(20);
            auto* dialog = panel->findChild<QDialog*>(entry.second);
            ASSERT_NE(nullptr, dialog);
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->isModal());
            const QImage image = dialog->grab().toImage();
            EXPECT_EQ(QColor(QStringLiteral("#151515")), image.pixelColor(2, 2));
            bool hasClose = false;
            for (auto* control : dialog->findChildren<QPushButton*>()) {
                hasClose |= control->text() == QStringLiteral("Close") && control->isVisible();
            }
            EXPECT_TRUE(hasClose);
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            break;
        }
    }
}

} // namespace
