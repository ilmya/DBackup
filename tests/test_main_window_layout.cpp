#include "gui/main_window.h"
#include "gui/theme.h"
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QTabBar>
#include <gtest/gtest.h>

TEST(MainWindowLayout, StartsOnBackupAndFitsMinimumSize) {
    MainWindow window;
    window.resize(960, 650);
    window.show();
    QApplication::processEvents();
    auto *pages = window.findChild<QStackedWidget *>();
    ASSERT_NE(pages, nullptr);
    ASSERT_EQ(pages->count(), 5);
    EXPECT_EQ(pages->currentIndex(), 0);
    auto *backup = qobject_cast<QScrollArea *>(pages->widget(0));
    ASSERT_NE(backup, nullptr);
    EXPECT_EQ(backup->horizontalScrollBar()->maximum(), 0);
    EXPECT_EQ(backup->verticalScrollBar()->maximum(), 0);
    for (int index = 1; index < pages->count(); ++index) {
        pages->setCurrentIndex(index);
        QApplication::processEvents();
        auto *page = qobject_cast<QScrollArea *>(pages->widget(index));
        ASSERT_NE(page, nullptr);
        EXPECT_EQ(page->horizontalScrollBar()->maximum(), 0) << index;
        EXPECT_EQ(page->verticalScrollBar()->maximum(), 0) << index;
    }
    auto *tabs = window.findChild<QTabBar *>("mainNavigation");
    ASSERT_NE(tabs, nullptr); EXPECT_EQ(tabs->count(), 4);
    ASSERT_FALSE(window.menuBar()->actions().empty());
    auto *settings = window.menuBar()->actions().first()->menu();
    ASSERT_NE(settings, nullptr); ASSERT_FALSE(settings->actions().empty());
    tabs->setCurrentIndex(1);
    EXPECT_EQ(pages->currentIndex(), 1);
    settings->actions().first()->trigger();
    EXPECT_EQ(pages->currentIndex(), 4);
    tabs->tabBarClicked(1);
    EXPECT_EQ(pages->currentIndex(), 1);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setStyleSheet(DBackupTheme::styleSheet());
    QCoreApplication::setOrganizationName("DBackup Test");
    QCoreApplication::setApplicationName("Layout Test");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
