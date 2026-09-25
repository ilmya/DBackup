#pragma once

#include <QMainWindow>
#include <QStringList>
#include <atomic>
#include "packer.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;
class QSystemTrayIcon;
class QCloseEvent;
class JobManager;
class RemoteClient;
class RemoteRepositoryService;

class MainWindow final : public QMainWindow {
 public:
    explicit MainWindow(QWidget *parent = nullptr);
 protected:
    void closeEvent(QCloseEvent *event) override;

 private:
    QWidget *createDashboardPage();
    QWidget *createBackupPage();
    QWidget *createRestorePage();
    QWidget *createSchedulePage();
    QWidget *createHistoryPage();
    QWidget *createStoragePage();
    QWidget *createSettingsPage();
    QWidget *createPageShell(const QString &title, const QString &subtitle, QVBoxLayout **content);
    void addSources(bool directory);
    void removeSelectedSources();
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
    QListWidget *sourceList_ = nullptr;
    QLabel *sourceSummary_ = nullptr;
    QLineEdit *backupTarget_ = nullptr;
    QComboBox *backupMode_ = nullptr;
    QSpinBox *compression_ = nullptr;
    QLineEdit *backupPassword_ = nullptr;
    QLabel *filterSummary_ = nullptr;
    QLabel *previewSummary_ = nullptr;
    QPushButton *backupButton_ = nullptr;
    QLineEdit *archivePath_ = nullptr;
    QLineEdit *restoreTarget_ = nullptr;
    QLineEdit *restorePassword_ = nullptr;
    QPushButton *restoreButton_ = nullptr;
    QPlainTextEdit *activityLog_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *progressDetails_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    JobManager *jobs_ = nullptr;
    RemoteClient *remote_ = nullptr;
    RemoteRepositoryService *remoteRepository_ = nullptr;
    std::atomic_bool cancelled_{false};
    bool quitting_ = false;
    FilterOptions filters_;
    bool busy_ = false;
};
