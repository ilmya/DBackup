#include "gui/main_window.h"
#include "gui/theme.h"
#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeView>
#include <gtest/gtest.h>

TEST(MainWindowLayout, StartsOnBackupAndFitsMinimumSize) {
    MainWindow window;
    EXPECT_EQ(window.size(), QSize(920, 650));
    window.resize(860, 600);
    window.show();
    QApplication::processEvents();
    auto* pages = window.findChild<QStackedWidget*>();
    auto* log = window.findChild<QPlainTextEdit*>("activityLog");
    auto* dock = window.findChild<QDockWidget*>("activityLogDock");
    ASSERT_NE(log, nullptr);
    ASSERT_NE(dock, nullptr);
    EXPECT_GE(log->height(), 100);
    EXPECT_GT(log->maximumHeight(), log->minimumHeight());
    ASSERT_NE(pages, nullptr);
    ASSERT_EQ(pages->count(), 5);
    EXPECT_EQ(pages->currentIndex(), 0);
    auto* backup = qobject_cast<QScrollArea*>(pages->widget(0));
    ASSERT_NE(backup, nullptr);
    auto* output = window.findChild<QWidget*>("backupOutputPanel");
    auto* filter = window.findChild<QWidget*>("backupFilterPanel");
    auto* tree = window.findChild<QTreeView*>("backupSourceTree");
    auto* splitter = window.findChild<QSplitter*>("backupColumns");
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(splitter, nullptr);
    EXPECT_FALSE(tree->isHeaderHidden());
    EXPECT_FALSE(tree->isColumnHidden(1));
    EXPECT_FALSE(tree->isColumnHidden(3));
    EXPECT_TRUE(tree->isColumnHidden(2));
    EXPECT_EQ(splitter->orientation(), Qt::Horizontal);
    EXPECT_NE(window.findChild<QWidget*>("selectedSourceInfo"), nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(filter, nullptr);
    EXPECT_TRUE(output->isAncestorOf(filter));
    EXPECT_LE(filter->mapTo(output, QPoint(0, filter->height())).y(), output->height());
    EXPECT_EQ(backup->horizontalScrollBar()->maximum(), 0);
    EXPECT_EQ(backup->verticalScrollBar()->maximum(), 0);
    for (int index = 1; index < pages->count(); ++index) {
        pages->setCurrentIndex(index);
        QApplication::processEvents();
        auto* page = qobject_cast<QScrollArea*>(pages->widget(index));
        ASSERT_NE(page, nullptr);
        EXPECT_EQ(page->horizontalScrollBar()->maximum(), 0) << index;
        EXPECT_EQ(page->verticalScrollBar()->maximum(), 0) << index;
    }
    auto* tabs = window.findChild<QTabBar*>("mainNavigation");
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->count(), 4);
    ASSERT_FALSE(window.menuBar()->actions().empty());
    auto* settings = window.menuBar()->actions().first()->menu();
    ASSERT_NE(settings, nullptr);
    ASSERT_FALSE(settings->actions().empty());
    tabs->setCurrentIndex(1);
    EXPECT_EQ(pages->currentIndex(), 1);
    settings->actions().first()->trigger();
    EXPECT_EQ(pages->currentIndex(), 4);
    tabs->tabBarClicked(1);
    EXPECT_EQ(pages->currentIndex(), 1);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setStyleSheet(DBackupTheme::styleSheet());
    QCoreApplication::setOrganizationName("DBackup Test");
    QCoreApplication::setApplicationName("Layout Test");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
