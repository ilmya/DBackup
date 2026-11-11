#include "gui/backup_options.h"
#include "gui/filter_dialog.h"
#include "gui/plan_result.h"
#include "gui/restore_options.h"
#include "gui/source_tree.h"
#include "gui/theme.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QTimeZone>
#include <gtest/gtest.h>
#include <memory>

TEST(RestoreOptions, DeviceRestorationIsEnabledByDefault) {
    std::unique_ptr<QCheckBox> option(createDeviceRestoreOption());
    EXPECT_TRUE(option->isChecked());
    EXPECT_TRUE(OperationContext{}.allowDeviceNodes);
}

TEST(BackupOptions, MapsSelectionsAndKeepsModesIndependent) {
    BackupOptionsWidget widget;
    auto* custom = widget.findChild<QRadioButton*>("packFormat0");
    auto* tar = widget.findChild<QRadioButton*>("packFormat1");
    auto* lz = widget.findChild<QRadioButton*>("compression2");
    auto* compression = widget.findChild<QCheckBox*>("compressionEnabled");
    auto* encryption = widget.findChild<QCheckBox*>("encryptionEnabled");
    auto* password = widget.findChild<QLineEdit*>("backupPassword");
    auto* full = widget.findChild<QRadioButton*>("backupMode0");
    auto* incremental = widget.findChild<QRadioButton*>("backupMode1");
    ASSERT_TRUE(custom && tar && lz && compression && encryption && password && full &&
                incremental);
    EXPECT_EQ(widget.options().format, PackFormat::Custom);
    EXPECT_EQ(widget.options().compression, Compression::Deflate);
    EXPECT_FALSE(widget.encryptionEnabled());
    EXPECT_FALSE(password->isEnabled());
    tar->setChecked(true);
    lz->setChecked(true);
    EXPECT_FALSE(custom->isChecked());
    EXPECT_EQ(widget.options().format, PackFormat::Tar);
    EXPECT_EQ(widget.options().compression, Compression::Lz77);
    compression->setChecked(false);
    EXPECT_EQ(widget.options().compression, Compression::None);
    EXPECT_FALSE(lz->isEnabled());
    encryption->setChecked(true);
    password->setText("test-password");
    EXPECT_TRUE(password->isEnabled());
    EXPECT_EQ(widget.options().password, "test-password");
    incremental->setChecked(true);
    EXPECT_TRUE(widget.incremental());
    EXPECT_FALSE(tar->isEnabled());
    EXPECT_FALSE(compression->isEnabled());
    EXPECT_FALSE(encryption->isEnabled());
    EXPECT_TRUE(password->isEnabled());
    full->setChecked(true);
    EXPECT_FALSE(widget.incremental());
    EXPECT_TRUE(tar->isChecked());
    EXPECT_EQ(widget.options().compression, Compression::None);
    EXPECT_TRUE(encryption->isChecked());
}

TEST(BackupOptions, FitsCompactWidth) {
    BackupOptionsWidget widget;
    widget.setStyleSheet(DBackupTheme::styleSheet());
    widget.resize(430, widget.sizeHint().height());
    widget.show();
    QApplication::processEvents();
    EXPECT_LE(widget.minimumSizeHint().width(), 430);
    for (auto* radio : widget.findChildren<QRadioButton*>()) {
        if (!radio->isVisible())
            continue;
        const QPoint origin = radio->mapTo(&widget, QPoint(0, 0));
        EXPECT_GE(origin.x(), 0);
        EXPECT_LE(origin.x() + radio->width(), widget.width());
    }
}

TEST(BackupOptions, SimpleBackupOnlyAllowsArchive) {
    BackupOptionsWidget widget(nullptr, false);
    auto* incremental = widget.findChild<QRadioButton*>("backupMode1");
    ASSERT_NE(incremental, nullptr);
    EXPECT_TRUE(incremental->parentWidget()->isHidden());
    widget.setOptions(true, true, PackFormat::Tar, Compression::None);
    EXPECT_FALSE(widget.incremental());
    BackupOptionsWidget plan;
    plan.setOptions(true, true, PackFormat::Tar, Compression::None);
    EXPECT_TRUE(plan.incremental());
}

TEST(SourceTree, SupportsNestedExclusionsAndReinclusions) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.path() + "/source";
    ASSERT_TRUE(QDir().mkpath(root + "/excluded/selected"));
    QFile file(root + "/file.txt");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    SourceTreeModel model;
    const QModelIndex directory = model.index(root);
    const QModelIndex excluded = model.index(root + "/excluded");
    const QModelIndex selected = model.index(root + "/excluded/selected");
    const QModelIndex regular = model.index(root + "/file.txt");
    ASSERT_TRUE(directory.isValid() && excluded.isValid() && selected.isValid() &&
                regular.isValid());
    ASSERT_TRUE(model.setData(directory, Qt::Checked, Qt::CheckStateRole));
    QFile added(root + "/added.txt");
    ASSERT_TRUE(added.open(QIODevice::WriteOnly));
    added.close();
    const QModelIndex addedIndex = model.index(root + "/added.txt");
    ASSERT_TRUE(addedIndex.isValid());
    EXPECT_EQ(model.data(addedIndex, Qt::CheckStateRole).toInt(), int(Qt::Checked));
    ASSERT_TRUE(model.setData(excluded, Qt::Unchecked, Qt::CheckStateRole));
    ASSERT_TRUE(model.setData(selected, Qt::Checked, Qt::CheckStateRole));
    EXPECT_EQ(model.data(directory, Qt::CheckStateRole).toInt(), int(Qt::PartiallyChecked));
    EXPECT_EQ(model.data(excluded, Qt::CheckStateRole).toInt(), int(Qt::PartiallyChecked));
    EXPECT_EQ(model.data(regular, Qt::CheckStateRole).toInt(), int(Qt::Checked));
    EXPECT_EQ(model.roots(), QStringList{root});
    EXPECT_EQ(model.rules().size(), 3u);
    ASSERT_TRUE(model.setData(directory, Qt::Unchecked, Qt::CheckStateRole));
    EXPECT_TRUE(model.rules().empty());
    ASSERT_TRUE(model.setData(regular, Qt::Checked, Qt::CheckStateRole));
    EXPECT_EQ(model.roots(), QStringList{root + "/file.txt"});
}

TEST(SourceTree, ShowsCompactSizeAndDateWithDetailedTooltips) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    QFile file(temp.filePath("sample.bin"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(QByteArray(1536, 'a')), 1536);
    file.close();
    SourceTreeModel model;
    const QModelIndex name = model.index(file.fileName());
    ASSERT_TRUE(name.isValid());
    const QModelIndex size = name.siblingAtColumn(1), date = name.siblingAtColumn(3);
    EXPECT_EQ(model.data(size, Qt::DisplayRole).toString(), QStringLiteral("1.5K"));
    EXPECT_TRUE(model.data(size, Qt::ToolTipRole).toString().contains("1536"));
    EXPECT_EQ(model.data(date, Qt::DisplayRole).toString(),
              QFileInfo(file).lastModified().toString("yyyy-MM-dd"));
    EXPECT_EQ(model.data(date, Qt::ToolTipRole).toString(),
              QFileInfo(file).lastModified().toString("yyyy-MM-dd HH:mm:ss"));
}

TEST(FilterDialog, PreservesConditionsAndUsesLocalCalendarDays) {
    FilterOptions input;
    input.pathContains = "docs";
    input.nameContains = "report";
    input.ownerContains = "alice";
    input.extensions = {".txt", ".md"};
    input.type = EntryTypeFilter::FilesOnly;
    input.typeMask = 0x51;
    input.minSize = 1024;
    input.maxSize = 8192;
    input.pathRules = {{true, "docs/*"}, {false, "docs/private/*"}};
    input.afterMtimeMs =
        QDateTime(QDate(2026, 1, 2), QTime(0, 0), QTimeZone::systemTimeZone()).toMSecsSinceEpoch();
    input.beforeMtimeMs =
        QDateTime(QDate(2026, 1, 3), QTime(23, 59, 59, 999), QTimeZone::systemTimeZone())
            .toMSecsSinceEpoch();
    FilterDialog dialog(input);
    const auto result = dialog.options();
    EXPECT_EQ(result.pathContains, input.pathContains);
    EXPECT_EQ(result.nameContains, input.nameContains);
    EXPECT_EQ(result.ownerContains, input.ownerContains);
    EXPECT_EQ(result.extensions, input.extensions);
    EXPECT_EQ(result.type, input.type);
    EXPECT_EQ(result.minSize, input.minSize);
    EXPECT_EQ(result.maxSize, input.maxSize);
    EXPECT_EQ(result.typeMask, input.typeMask);
    ASSERT_EQ(result.pathRules.size(), 2u);
    EXPECT_TRUE(result.pathRules[0].include);
    EXPECT_EQ(result.pathRules[0].pattern, "docs/*");
    EXPECT_FALSE(result.pathRules[1].include);
    EXPECT_EQ(result.pathRules[1].pattern, "docs/private/*");
    EXPECT_EQ(result.afterMtimeMs, input.afterMtimeMs);
    EXPECT_EQ(result.beforeMtimeMs, input.beforeMtimeMs);
}

TEST(PlanResult, RealtimeSummaryHighlightsChangedFilesAndRetainsPartialNotice) {
    BackupJob job;
    job.changedPaths = {"/source/changed.txt"};
    const QString unrelated =
        QString::fromUtf8("无法读取来源: /source/private.txt: Permission denied");
    const QString result = realtimeResultDetail(job, unrelated, true);
    EXPECT_TRUE(result.contains("/source/changed.txt"));
    EXPECT_FALSE(result.contains("/source/private.txt"));
    EXPECT_TRUE(result.contains(QString::fromUtf8("其他未备份项")));
    const QString changedError =
        QString::fromUtf8("无法读取来源: /source/changed.txt: Permission denied");
    EXPECT_TRUE(realtimeResultDetail(job, changedError, true).contains(changedError));
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
