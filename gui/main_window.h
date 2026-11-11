#pragma once

#include "core/archive.h"
#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <atomic>

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
class QDialog;
class JobManager;
struct BackupJob;

class MainWindow final : public QMainWindow {
  public:
    // 创建主窗口、功能页、活动日志及自动任务管理器。
    explicit MainWindow(QWidget* parent = nullptr);

  protected:
    // 关闭主窗口时隐藏到托盘或按退出状态结束程序。
    void closeEvent(QCloseEvent* event) override;

  private:
    // 创建来源树、输出格式和筛选入口所在的备份页。
    QWidget* createBackupPage();
    // 创建归档文件、目标目录和恢复选项所在的恢复页。
    QWidget* createRestorePage();
    // 创建定时/实时任务列表及编辑操作所在的页面。
    QWidget* createSchedulePage();
    // 创建本地增量仓库快照列表和恢复入口。
    QWidget* createHistoryPage(const BackupJob& job);
    // 创建凭据和日志等通用设置页。
    QWidget* createSettingsPage();
    // 创建统一标题、内容布局和页边距的功能页容器。
    QWidget* createPageShell(const QString& title, QVBoxLayout** content);
    // 选择并验证独立归档或仓库的输出目标。
    void chooseBackupTarget();
    // 打开筛选配置并刷新当前筛选摘要。
    void editFilters();
    // 扫描所选来源并展示文件类型、大小与不可读对象统计。
    void previewFilters();
    // 按当前来源和选项启动独立归档备份。
    void startBackup();
    // 选择用于恢复的归档文件。
    void chooseArchive();
    // 选择恢复目标目录。
    void chooseRestoreFolder();
    // 校验密码和目标后启动归档恢复。
    void startRestore();
    // 更新操作期间按钮状态和状态栏提示。
    void setBusy(bool busy, const QString& message = {});
    // 将后台进度映射到进度条和阶段说明。
    void updateProgress(const OperationProgress& progress);
    // 向可滚动活动日志追加一条状态或错误记录。
    void appendLog(const QString& message, bool error = false);
    // 根据树模型中的来源选择更新来源计数。
    void updateSourceSummary();
    // 根据当前条件更新筛选摘要文本。
    void updateFilterSummary();
    // 解析来源树选择及过滤规则，生成执行器使用的来源列表。
    std::vector<std::string> sourcePaths() const;

    QStackedWidget* pages_ = nullptr;
    SourceTreeModel* sourceModel_ = nullptr;
    QTreeView* sourceTree_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QLineEdit* backupTarget_ = nullptr;
    BackupOptionsWidget* backupOptions_ = nullptr;
    QLabel* filterSummary_ = nullptr;
    QLabel* previewSummary_ = nullptr;
    QPushButton* backupButton_ = nullptr;
    QLineEdit* archivePath_ = nullptr;
    QLineEdit* restoreTarget_ = nullptr;
    QLineEdit* restorePassword_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPlainTextEdit* activityLog_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* progressDetails_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    JobManager* jobs_ = nullptr;
    QPointer<QDialog> planDialog_;
    QPointer<QDialog> planDetails_;
    std::atomic_bool cancelled_{false};
    bool quitting_ = false;
    FilterOptions filters_;
    bool busy_ = false;
};
