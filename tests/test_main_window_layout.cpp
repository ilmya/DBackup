// 验证主窗口布局
#include "gui/main_window.h"
#include "gui/theme.h"
#include "automation/job_manager.h"
#include "automation/plan_executor.h"
#include "core/security/credential_store.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QDockWidget>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
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
    ASSERT_EQ(pages->count(), 4);
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
    EXPECT_EQ(window.findChild<QWidget*>("selectedSourceInfo"), nullptr);
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
    EXPECT_EQ(tabs->count(), 3);
    ASSERT_FALSE(window.menuBar()->actions().empty());
    auto* settings = window.menuBar()->actions().first()->menu();
    ASSERT_NE(settings, nullptr);
    ASSERT_FALSE(settings->actions().empty());
    tabs->setCurrentIndex(1);
    EXPECT_EQ(pages->currentIndex(), 1);
    settings->actions().first()->trigger();
    EXPECT_EQ(pages->currentIndex(), 3);
    tabs->tabBarClicked(1);
    EXPECT_EQ(pages->currentIndex(), 1);
}

TEST(MainWindowLayout, PlanEditorIsIndependentAndEnforcesRunOptions) {
    MainWindow window;
    window.show();
    auto* pages = window.findChild<QStackedWidget*>();
    ASSERT_NE(pages, nullptr);
    pages->setCurrentIndex(2);
    auto* add = window.findChild<QPushButton*>("addPlan");
    ASSERT_NE(add, nullptr);
    add->click();
    QApplication::processEvents();
    QDialog* editor = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "planEditor")
            editor = qobject_cast<QDialog*>(widget);
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->windowModality(), Qt::NonModal);
    EXPECT_TRUE(window.isEnabled());
    auto* scheduled = editor->findChild<QRadioButton*>("planScheduled");
    auto* realtime = editor->findChild<QRadioButton*>("planRealtime");
    auto* cron = editor->findChild<QLineEdit*>("planCron");
    auto* full = editor->findChild<QRadioButton*>("backupMode0");
    auto* tarIncremental = editor->findChild<QRadioButton*>("backupMode1");
    auto* tarFormat = editor->findChild<QRadioButton*>("packFormat1");
    auto* customFormat = editor->findChild<QRadioButton*>("packFormat0");
    auto* compression = editor->findChild<QCheckBox*>("compressionEnabled");
    ASSERT_TRUE(scheduled && realtime && cron && full && tarIncremental && tarFormat && customFormat && compression);
    EXPECT_TRUE(scheduled->isChecked());
    EXPECT_TRUE(cron->isEnabled());
    realtime->setChecked(true);
    EXPECT_FALSE(scheduled->isChecked());
    EXPECT_FALSE(cron->isEnabled());
    EXPECT_TRUE(tarIncremental->isChecked());
    EXPECT_FALSE(full->isEnabled());
    EXPECT_TRUE(tarFormat->isEnabled());
    EXPECT_TRUE(customFormat->isEnabled());
    customFormat->setChecked(true);
    EXPECT_TRUE(customFormat->isChecked());
    tarFormat->setChecked(true);
    EXPECT_TRUE(tarFormat->isChecked());
    EXPECT_TRUE(compression->isEnabled());
    scheduled->setChecked(true);
    EXPECT_FALSE(realtime->isChecked());
    EXPECT_TRUE(cron->isEnabled());
    EXPECT_TRUE(full->isEnabled());
    EXPECT_TRUE(full->isChecked());
    add->click();
    int editors = 0;
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "planEditor" && widget->isVisible())
            ++editors;
    EXPECT_EQ(editors, 1);
    editor->reject();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

TEST(MainWindowLayout, PlanDetailsLoadsSelectedIncrementalHistory) {
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    ASSERT_TRUE(QDir().mkpath(data.filePath("source")));
    QFile file(data.filePath("source/file"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("backup"), 6);
    file.close();
    BackupJob job;
    job.id = "layout-history";
    job.name = "History plan";
    job.sources = {data.filePath("source")};
    job.destination = data.path();
    job.tarIncremental = true;
    job.encrypted = false;
    job.scheduled = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    const auto result = executePlan(job, "");
    ASSERT_TRUE(result.success) << result.detail;
    JobManager manager;
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    MainWindow window;
    window.show();
    auto* plans = window.findChild<QTableWidget*>("planTable");
    ASSERT_NE(plans, nullptr);
    EXPECT_EQ(plans->editTriggers(), QAbstractItemView::NoEditTriggers);
    int row = -1;
    for (int i = 0; i < plans->rowCount(); ++i)
        if (plans->item(i, 0)->data(Qt::UserRole).toString() == job.id) row = i;
    ASSERT_GE(row, 0);
    plans->selectRow(row);
    auto* button = window.findChild<QPushButton*>("planDetailsButton");
    ASSERT_NE(button, nullptr);
    button->click();
    auto* dialog = window.findChild<QDialog*>("planDetails");
    ASSERT_NE(dialog, nullptr);
    auto* load = dialog->findChild<QPushButton*>("loadPlanHistory");
    auto* history = dialog->findChild<QTableWidget*>("planHistoryTable");
    ASSERT_TRUE(load && history);
    EXPECT_EQ(history->editTriggers(), QAbstractItemView::NoEditTriggers);
    QElapsedTimer timer; timer.start();
    while (history->rowCount() == 0 && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_EQ(history->rowCount(), 1);
    EXPECT_EQ(history->item(0, 2)->text(), QString::fromUtf8("添加：file"));
    ASSERT_TRUE(manager.remove(job.id, error));
}

TEST(MainWindowLayout, EncryptedHistoryLoadsWithoutSavedPassword) {
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    ASSERT_TRUE(QDir().mkpath(data.filePath("source")));
    QFile file(data.filePath("source/file"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("backup"), 6);
    file.close();
    BackupJob job;
    job.id = "layout-encrypted-history";
    job.name = "Encrypted history";
    job.sources = {data.filePath("source")};
    job.destination = data.path();
    job.tarIncremental = true;
    job.encrypted = true;
    job.scheduled = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    const auto result = executePlan(job, "secret");
    ASSERT_TRUE(result.success) << result.detail;
    JobManager manager;
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    MainWindow window;
    window.show();
    auto* plans = window.findChild<QTableWidget*>("planTable");
    ASSERT_NE(plans, nullptr);
    int row = -1;
    for (int i = 0; i < plans->rowCount(); ++i)
        if (plans->item(i, 0)->data(Qt::UserRole).toString() == job.id) row = i;
    ASSERT_GE(row, 0);
    plans->selectRow(row);
    window.findChild<QPushButton*>("planDetailsButton")->click();
    auto* dialog = window.findChild<QDialog*>("planDetails");
    ASSERT_NE(dialog, nullptr);
    auto* password = dialog->findChild<QLineEdit*>("planHistoryPassword");
    auto* status = dialog->findChild<QLabel*>("planHistoryStatus");
    auto* load = dialog->findChild<QPushButton*>("loadPlanHistory");
    auto* history = dialog->findChild<QTableWidget*>("planHistoryTable");
    ASSERT_TRUE(password && status && load && history);
    QElapsedTimer timer; timer.start();
    while (history->rowCount() == 0 && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    EXPECT_TRUE(password->isVisible());
    EXPECT_FALSE(load->isVisible());
    EXPECT_TRUE(status->text().isEmpty());
    EXPECT_EQ(history->rowCount(), 1);
    EXPECT_EQ(history->item(0, 2)->text(), QString::fromUtf8("添加：file"));
    std::string credentialError;
    ASSERT_TRUE(CredentialStore::save(("job/" + job.id).toStdString(), "secret", credentialError))
        << credentialError;
    window.findChild<QPushButton*>("planDetailsButton")->click();
    dialog = window.findChild<QDialog*>("planDetails");
    ASSERT_NE(dialog, nullptr);
    history = dialog->findChild<QTableWidget*>("planHistoryTable");
    ASSERT_NE(history, nullptr);
    timer.restart();
    while (history->rowCount() == 0 && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    EXPECT_EQ(history->rowCount(), 1);
    EXPECT_TRUE(dialog->findChild<QLineEdit*>("planHistoryPassword")->isVisible());
    EXPECT_EQ(dialog->findChild<QLineEdit*>("planHistoryPassword")->text(), "secret");
    EXPECT_FALSE(dialog->findChild<QPushButton*>("loadPlanHistory")->isVisible());
    EXPECT_TRUE(CredentialStore::remove(("job/" + job.id).toStdString(), credentialError));
    ASSERT_TRUE(manager.remove(job.id, error));
}

int main(int argc, char** argv) {
    QTemporaryDir appData;
    if (!appData.isValid()) return 1;
    qputenv("XDG_DATA_HOME", appData.path().toUtf8());
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setStyleSheet(DBackupTheme::styleSheet());
    QCoreApplication::setOrganizationName("DBackup Test");
    QCoreApplication::setApplicationName("Layout Test");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
