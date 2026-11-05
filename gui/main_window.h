#pragma once

#include <QMainWindow>
#include <QStringList>
#include <atomic>
#include <memory>
#include <QMap>
#include "core/archive.h"

class QLabel;
class QLineEdit;
class BackupOptionsWidget;
class QCheckBox;
class SourceTreeModel;
class QTreeView;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QSystemTrayIcon;
class QCloseEvent;
class JobManager;

class MainWindow final : public QMainWindow {
 public:
    explicit MainWindow(QWidget *parent = nullptr);
 protected:
    void closeEvent(QCloseEvent *event) override;

 private:
    QWidget *createBackupPage();
    QWidget *createRestorePage();
    QWidget *createSchedulePage();
    QWidget *createHistoryPage();
    QWidget *createSettingsPage();
    QWidget *createPageShell(const QString &title, QVBoxLayout **content);
    void chooseBackupTarget();
    void editFilters();
    void previewFilters();
    void startBackup();
    void chooseArchive();
    void chooseRestoreFolder();
    void startRestore();
    void setBusy(bool busy, const QString &message = {});
    void updateProgress(const OperationProgress &progress);
    void appendLog(const QString &message, bool error = false);
    void updateSourceSummary();
    void updateFilterSummary();
    std::vector<std::string> sourcePaths() const;

    QStackedWidget *pages_ = nullptr;
    SourceTreeModel *sourceModel_ = nullptr;
    QTreeView *sourceTree_ = nullptr;
    QLabel *sourceSummary_ = nullptr;
    QLineEdit *backupTarget_ = nullptr;
    BackupOptionsWidget *backupOptions_ = nullptr;
    QLabel *filterSummary_ = nullptr;
    QLabel *previewSummary_ = nullptr;
    QPushButton *backupButton_ = nullptr;
    QLineEdit *archivePath_ = nullptr;
    QLineEdit *restoreTarget_ = nullptr;
    QLineEdit *restorePassword_ = nullptr;
    QCheckBox *restoreDevices_ = nullptr;
    QPushButton *restoreButton_ = nullptr;
    QPlainTextEdit *activityLog_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *progressDetails_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    JobManager *jobs_ = nullptr;
    std::atomic_bool cancelled_{false};
    QMap<QString, std::shared_ptr<std::atomic_bool>> planCancellations_;
    bool quitting_ = false;
    FilterOptions filters_;
    bool busy_ = false;
};
