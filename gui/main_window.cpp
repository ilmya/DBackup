#include "gui/main_window.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>
#include <unistd.h>

#include "automation/job_manager.h"
#include "automation/plan_executor.h"
#include "core/credential_store.h"
#include "core/incremental_chain.h"
#include "core/md5.h"
#include "core/repository.h"
#include "gui/backup_options.h"
#include "gui/filter_dialog.h"
#include "gui/plan_result.h"
#include "gui/source_tree.h"

namespace {
// 创建统一样式的普通或主操作按钮。
QPushButton* button(const QString& text, bool primary = false) {
    auto* result = new QPushButton(text);
    if (primary)
        result->setProperty("primary", true);
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QString changeSummary(const std::vector<std::string>& added,
                      const std::vector<std::string>& modified,
                      const std::vector<std::string>& deleted) {
    QStringList groups;
    const auto addGroup = [&groups](const QString& label, const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        QStringList names;
        for (const auto& path : paths) {
            QString value = QString::fromUtf8(path.c_str());
            const auto slash = value.indexOf('/');
            if (slash >= 0) value = value.mid(slash + 1);
            names << value;
        }
        groups << label + names.join(QObject::tr("、"));
    };
    addGroup(QObject::tr("添加："), added);
    addGroup(QObject::tr("修改："), modified);
    addGroup(QObject::tr("删除："), deleted);
    return groups.isEmpty() ? QObject::tr("无文件变化") : groups.join(QObject::tr("；"));
}

QString displayPlanResult(const BackupJob& job) {
    QString result = job.lastResult;
    if (result.startsWith(QObject::tr("成功：/")))
        return QObject::tr("成功：备份完成，文件变化分类不可用");
    for (const auto& source : job.sources) {
        const QString prefix = QDir::cleanPath(source) + '/';
        result.replace(prefix, QString());
        result.replace(QDir::cleanPath(source), QFileInfo(source).fileName());
    }
    return result;
}

// 将文件大小转换为界面展示用的可读单位。
QString humanSize(uint64_t bytes) {
    static const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', unit == 0 ? 0 : 2) + " " + units[unit];
}

// 组合各文件类型的包含/排除计数和不可读来源摘要。
QString previewDetails(const BackupPreview& preview, const std::string& warnings) {
    const QStringList names = {QObject::tr("普通文件"),  QObject::tr("目录"),
                               QObject::tr("符号链接"),  QObject::tr("硬链接"),
                               QObject::tr("FIFO 管道"), QObject::tr("字符设备"),
                               QObject::tr("块设备"),    QObject::tr("Unix 套接字")};
    QString text = QObject::tr("类型                         包含 / 排除\n");
    for (int i = 0; i < names.size(); ++i)
        text += QObject::tr("%1：%2 / %3\n")
                    .arg(names[i])
                    .arg(preview.includedTypes[i])
                    .arg(preview.excludedTypes[i]);
    text += QObject::tr("\n普通文件总大小：%1").arg(humanSize(preview.includedBytes));
    if (preview.unreadableFiles)
        text += QObject::tr("\n其中 %1 个文件无法读取，备份时将跳过").arg(preview.unreadableFiles);
    if (!warnings.empty())
        text += QObject::tr("\n\n无法读取：\n%1").arg(QString::fromUtf8(warnings.c_str()));
    return text;
}

// 将操作阶段枚举转换成面向用户的简短状态文字。
QString stageName(OperationStage stage) {
    switch (stage) {
    case OperationStage::Scanning:
        return QObject::tr("正在扫描");
    case OperationStage::Reading:
        return QObject::tr("正在读取");
    case OperationStage::Compressing:
        return QObject::tr("正在压缩");
    case OperationStage::Encrypting:
        return QObject::tr("正在加密");
    case OperationStage::Writing:
        return QObject::tr("正在写入");
    case OperationStage::Restoring:
        return QObject::tr("正在恢复");
    case OperationStage::Completed:
        return QObject::tr("已完成");
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(geteuid() == 0 ? tr("DBackup · 备份管理器（root 模式）")
                                  : tr("DBackup · 备份管理器"));
    resize(920, 650);
    setMinimumSize(860, 600);
    auto* root = new QWidget;
    root->setObjectName("appRoot");
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    pages_ = new QStackedWidget;
    pages_->addWidget(createBackupPage());
    pages_->addWidget(createRestorePage());
    jobs_ = new JobManager(this);
    pages_->addWidget(createSchedulePage());
    pages_->addWidget(createSettingsPage());

    auto* tabs = new QTabBar;
    tabs->setObjectName("mainNavigation");
    for (const auto& name : {tr("备份"), tr("恢复"), tr("自动任务")})
        tabs->addTab(name);
    connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0)
            pages_->setCurrentIndex(index);
    });
    connect(tabs, &QTabBar::tabBarClicked, this, [this](int index) {
        if (index >= 0)
            pages_->setCurrentIndex(index);
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [tabs](int index) {
        tabs->setProperty("settingsActive", index >= tabs->count());
        tabs->style()->unpolish(tabs);
        tabs->style()->polish(tabs);
        tabs->update();
        if (index < tabs->count())
            tabs->setCurrentIndex(index);
    });
    auto* settingsMenu = menuBar()->addMenu(tr("设置"));
    settingsMenu->addAction(tr("应用设置"), this, [this] { pages_->setCurrentIndex(3); });
    layout->addWidget(tabs);
    layout->addWidget(pages_, 1);
    setCentralWidget(root);

    activityLog_ = new QPlainTextEdit;
    activityLog_->setObjectName("activityLog");
    activityLog_->setReadOnly(true);
    activityLog_->setPlaceholderText(tr("任务活动会显示在这里"));
    activityLog_->setMinimumHeight(100);
    auto* dock = new QDockWidget(tr("活动日志"), this);
    dock->setObjectName("activityLogDock");
    dock->setWidget(activityLog_);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    resizeDocks({dock}, {165}, Qt::Vertical);
    menuBar()->addMenu(tr("视图"))->addAction(dock->toggleViewAction());
    progressDetails_ = new QLabel(tr("就绪"));
    progressDetails_->setMinimumWidth(260);
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedWidth(180);
    progress_->setTextVisible(true);
    cancelButton_ = button(tr("取消"));
    cancelButton_->setVisible(false);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        cancelled_ = true;
        statusBar()->showMessage(tr("正在取消…"));
    });
    statusBar()->addPermanentWidget(progressDetails_, 1);
    statusBar()->addPermanentWidget(cancelButton_);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->showMessage(tr("就绪"));
    appendLog(tr("DBackup 已启动，核心备份服务就绪。"));
    if (!jobs_->loadError().isEmpty()) {
        appendLog(jobs_->loadError(), true);
        QMessageBox::warning(this, tr("计划读取失败"), jobs_->loadError());
    }
    auto showApp = [this] {
        showNormal();
        QWidget* window = QApplication::activeModalWidget();
        if (!window || (window != this && !isAncestorOf(window)))
            window = this;
        window->raise();
        window->activateWindow();
    };
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction(tr("打开 DBackup"), this, showApp);
    trayMenu->addAction(tr("暂停自动任务"), this, [this] { jobs_->pause(true); });
    trayMenu->addAction(tr("继续自动任务"), this, [this] { jobs_->pause(false); });
    trayMenu->addSeparator();
    trayMenu->addAction(tr("退出"), this, [this] {
        quitting_ = true;
        qApp->quit();
    });
    setWindowIcon(QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon));
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    tray_->setToolTip("DBackup");
    tray_->setContextMenu(trayMenu);
    tray_->show();
    connect(tray_, &QSystemTrayIcon::activated, this, [showApp](auto reason) {
        if (reason == QSystemTrayIcon::Trigger)
            showApp();
    });
    jobs_->setExecutor([this](const BackupJob& job) {
        appendLog(tr("开始执行计划：%1").arg(job.name));
        QThread* worker = QThread::create([this, job] {
            std::string password, error;
            const bool credentialOk =
                !job.encrypted ||
                CredentialStore::load(("job/" + job.id).toStdString(), password, error);
            OperationContext context;
            std::string lastPath;
            OperationStage lastStage = OperationStage::Completed;
            context.onProgress = [this, job, &lastPath,
                                  &lastStage](const OperationProgress& progress) {
                if (progress.currentPath == lastPath && progress.stage == lastStage)
                    return;
                lastPath = progress.currentPath;
                lastStage = progress.stage;
                const QString line = tr("计划“%1”：%2 %3")
                                         .arg(job.name, stageName(progress.stage),
                                              QString::fromUtf8(progress.currentPath.c_str()));
                QMetaObject::invokeMethod(
                    this, [this, line] { appendLog(line); }, Qt::QueuedConnection);
            };
            PlanResult result;
            if (credentialOk)
                result = executePlan(job, password, context);
            else
                result.detail = error;
            const bool ok = result.success;
            error = result.detail;
            QMetaObject::invokeMethod(
                this,
                [this, job, ok, error, result] {
                    const QString detail = QString::fromUtf8(error.c_str());
                    QString recent = detail;
                    if (ok && result.changesAvailable)
                        recent = changeSummary(result.added, result.modified, result.deleted);
                    else if (ok)
                        recent = tr("备份完成，文件变化分类不可用") +
                                 (result.partial ? tr("；部分文件未备份，详见活动日志") : QString());
                    jobs_->markFinished(job.id, ok, recent, result.partial, false);
                    appendLog(ok ? (result.partial
                                        ? tr("计划“%1”部分完成：%2")
                                              .arg(job.name, QString::fromUtf8(error.c_str()))
                                        : tr("计划“%1”完成：%2")
                                              .arg(job.name, QString::fromUtf8(error.c_str())))
                                 : tr("计划“%1”失败：%2")
                                       .arg(job.name, QString::fromUtf8(error.c_str())),
                              !ok || result.partial);
                },
                Qt::QueuedConnection);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    });
    jobs_->start();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (quitting_) {
        event->accept();
        return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        event->accept();
        qApp->quit();
        return;
    }
    hide();
    tray_->showMessage(tr("DBackup 正在后台运行"),
                       tr("定时和实时任务会继续执行，可从托盘菜单退出。"));
    event->ignore();
}

QWidget* MainWindow::createPageShell(const QString& title, QVBoxLayout** content) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 6, 14, 6);
    layout->setSpacing(5);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("pageTitle");
    layout->addWidget(titleLabel);
    *content = layout;
    scroll->setWidget(page);
    return scroll;
}

QWidget* MainWindow::createBackupPage() {
    QVBoxLayout* layout = nullptr;
    QWidget* page = createPageShell(tr("新建备份"), &layout);
    auto* columns = new QSplitter(Qt::Horizontal);
    columns->setObjectName("backupColumns");
    columns->setChildrenCollapsible(false);
    columns->setHandleWidth(6);

    auto* sources = new QGroupBox(tr("备份来源"));
    auto* sourceLayout = new QVBoxLayout(sources);
    sourceLayout->setContentsMargins(7, 7, 7, 7);
    sourceLayout->setSpacing(4);
    sourceModel_ = new SourceTreeModel(this);
    sourceTree_ = createSourceTree(sourceModel_);
    sourceTree_->setObjectName("backupSourceTree");
    sourceTree_->setHeaderHidden(false);
    sourceTree_->showColumn(1);
    sourceTree_->showColumn(3);
    sourceTree_->header()->setStretchLastSection(false);
    sourceTree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    sourceTree_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    sourceTree_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    sourceTree_->setColumnWidth(0, 175);
    sourceTree_->setColumnWidth(1, 58);
    sourceTree_->setColumnWidth(3, 95);
    sourceTree_->setMinimumHeight(110);
    sourceLayout->addWidget(sourceTree_, 1);
    sourceSummary_ = new QLabel(tr("勾选文件或文件夹"));
    sourceSummary_->setProperty("muted", true);
    sourceLayout->addWidget(sourceSummary_);
    connect(sourceModel_, &QAbstractItemModel::layoutChanged, this,
            [this] { updateSourceSummary(); });
    columns->addWidget(sources);

    auto* target = new QGroupBox(tr("输出设置"));
    target->setObjectName("backupOutputPanel");
    auto* targetLayout = new QVBoxLayout(target);
    targetLayout->setContentsMargins(7, 7, 7, 7);
    targetLayout->setSpacing(4);
    auto* targetRow = new QHBoxLayout;
    backupTarget_ = new QLineEdit;
    backupTarget_->setPlaceholderText(tr("保存位置"));
    auto* browse = button(tr("浏览…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::chooseBackupTarget);
    targetRow->addWidget(backupTarget_, 1);
    targetRow->addWidget(browse);
    targetLayout->addLayout(targetRow);
    backupOptions_ = new BackupOptionsWidget(nullptr, false);
    targetLayout->addWidget(backupOptions_);
    auto* filter = new QWidget;
    filter->setObjectName("backupFilterPanel");
    auto* filterLayout = new QVBoxLayout(filter);
    filterLayout->setContentsMargins(0, 4, 0, 0);
    filterLayout->setSpacing(3);
    auto* summaries = new QVBoxLayout;
    summaries->setSpacing(2);
    filterSummary_ = new QLabel;
    filterSummary_->setWordWrap(true);
    filterSummary_->setProperty("muted", true);
    previewSummary_ = new QLabel(tr("尚未扫描"));
    previewSummary_->setProperty("muted", true);
    summaries->addWidget(filterSummary_);
    summaries->addWidget(previewSummary_);
    auto* edit = button(tr("配置…"));
    auto* preview = button(tr("扫描预览"));
    connect(edit, &QPushButton::clicked, this, &MainWindow::editFilters);
    connect(preview, &QPushButton::clicked, this, &MainWindow::previewFilters);
    filterLayout->addLayout(summaries);
    auto* run = new QHBoxLayout;
    run->addWidget(edit);
    run->addWidget(preview);
    run->addStretch();
    backupButton_ = button(tr("开始备份"), true);
    backupButton_->setMinimumWidth(112);
    connect(backupButton_, &QPushButton::clicked, this, &MainWindow::startBackup);
    run->addWidget(backupButton_);
    filterLayout->addLayout(run);
    targetLayout->addStretch();
    targetLayout->addWidget(filter);
    columns->addWidget(target);
    columns->setStretchFactor(0, 1);
    columns->setStretchFactor(1, 1);
    columns->setSizes({390, 500});
    layout->addWidget(columns, 1);
    updateFilterSummary();
    return page;
}

QWidget* MainWindow::createRestorePage() {
    QVBoxLayout* layout = nullptr;
    QWidget* page = createPageShell(tr("恢复数据"), &layout);
    auto* form = new QGroupBox(tr("恢复设置"));
    form->setMaximumWidth(820);
    auto* grid = new QGridLayout(form);
    archivePath_ = new QLineEdit;
    restoreTarget_ = new QLineEdit;
    restorePassword_ = new QLineEdit;
    restorePassword_->setEchoMode(QLineEdit::Password);
    auto* archiveBrowse = button(tr("浏览…"));
    auto* targetBrowse = button(tr("浏览…"));
    archiveBrowse->setMaximumWidth(95);
    targetBrowse->setMaximumWidth(95);
    connect(archiveBrowse, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(targetBrowse, &QPushButton::clicked, this, &MainWindow::chooseRestoreFolder);
    grid->addWidget(new QLabel(tr("归档文件")), 0, 0);
    grid->addWidget(archivePath_, 0, 1);
    grid->addWidget(archiveBrowse, 0, 2);
    grid->addWidget(new QLabel(tr("恢复到")), 1, 0);
    grid->addWidget(restoreTarget_, 1, 1);
    grid->addWidget(targetBrowse, 1, 2);
    grid->addWidget(new QLabel(tr("解密密码")), 2, 0);
    grid->addWidget(restorePassword_, 2, 1);
    restoreButton_ = button(tr("开始恢复"), true);
    restoreButton_->setMinimumWidth(95);
    restoreButton_->setMaximumWidth(120);
    grid->addWidget(restoreButton_, 3, 2);
    layout->addWidget(form);
    connect(restoreButton_, &QPushButton::clicked, this, &MainWindow::startRestore);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::createSchedulePage() {
    QVBoxLayout* layout = nullptr;
    QWidget* page = createPageShell(tr("备份计划"), &layout);
    auto* table = new QTableWidget(0, 7);
    table->setAlternatingRowColors(true);
    table->setHorizontalHeaderLabels(
        {tr("名称"), tr("类型"), tr("方式"), tr("下次运行"), tr("状态"), tr("最近成功"), tr("最近结果")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    auto refresh = [this, table] {
        table->setRowCount(0);
        for (const auto& job : jobs_->jobs()) {
            int row = table->rowCount();
            table->insertRow(row);
            QString mode = job.scheduled ? (job.realtime ? tr("定时 + 实时") : tr("定时"))
                                         : (job.realtime ? tr("实时") : tr("手动"));
            QStringList values = {job.name,
                                  job.tarIncremental ? tr("增量归档") :
                                      (job.repository ? tr("版本仓库") : tr("独立归档")),
                                  mode,
                                  job.nextRun.toString("yyyy-MM-dd HH:mm"),
                                  !job.enabled ? tr("停用")
                                               : (job.realtime ? job.realtimeStatus : tr("启用")),
                                  job.lastSuccess.isValid() ? job.lastSuccess.toString("yyyy-MM-dd HH:mm") : tr("暂无"),
                                  displayPlanResult(job)};
            for (int c = 0; c < values.size(); ++c) {
                auto* item = new QTableWidgetItem(values[c]);
                item->setData(Qt::UserRole, job.id);
                item->setToolTip(values[c]);
                table->setItem(row, c, item);
            }
        }
    };
    jobs_->setOnChanged(refresh);
    layout->addWidget(table);
    auto* actions = new QHBoxLayout;
    auto *add = button(tr("新建"), true), *edit = button(tr("编辑")), *run = button(tr("立即运行")),
         *toggle = button(tr("启用/停用")), *remove = button(tr("删除")),
         *details = button(tr("详情"));
    add->setObjectName("addPlan");
    details->setObjectName("planDetailsButton");
    table->setObjectName("planTable");
    for (auto* item : {add, edit, run, toggle, remove, details})
        actions->addWidget(item);
    actions->addStretch();
    layout->addLayout(actions);
    auto selected = [this, table]() -> BackupJob {
        if (table->currentRow() < 0)
            return {};
        const QString id = table->item(table->currentRow(), 0)->data(Qt::UserRole).toString();
        for (const auto& job : jobs_->jobs())
            if (job.id == id)
                return job;
        return {};
    };
    auto configure = [this, refresh](BackupJob job, bool isNew) {
        if (planDialog_) {
            planDialog_->showNormal();
            planDialog_->raise();
            planDialog_->activateWindow();
            return;
        }
        auto* dialog = new QDialog(nullptr);
        dialog->setObjectName("planEditor");
        planDialog_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(isNew ? tr("新建备份计划") : tr("编辑备份计划"));
        dialog->resize(900, 650);
        connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
        auto* outer = new QVBoxLayout(dialog);
        auto* body = new QHBoxLayout;
        auto* model = new SourceTreeModel(dialog);
        auto selectedRules = isNew ? sourceModel_->rules() : job.filter.sourceRules;
        if (!isNew && selectedRules.empty())
            for (const auto& source : job.sources)
                selectedRules.push_back({source.toUtf8().toStdString(), true});
        model->setRules(selectedRules);
        auto* tree = createSourceTree(model, dialog);
        body->addWidget(tree, 2);
        auto* formWidget = new QWidget(dialog);
        auto* form = new QFormLayout(formWidget);
        auto* name = new QLineEdit(isNew ? tr("备份计划") : job.name);
        auto* target = new QLineEdit(job.destination);
        auto* choose = button(tr("浏览…"));
        auto* targetRow = new QHBoxLayout;
        targetRow->addWidget(target);
        targetRow->addWidget(choose);
        connect(choose, &QPushButton::clicked, dialog, [dialog, target] {
            const auto path = QFileDialog::getExistingDirectory(
                dialog, QObject::tr("选择保存目录"), target->text());
            if (!path.isEmpty())
                target->setText(path);
        });
        auto* options = new BackupOptionsWidget(dialog);
        if (isNew) {
            const auto current = backupOptions_->options();
            options->setOptions(backupOptions_->incremental(), backupOptions_->encryptionEnabled(),
                                current.format, current.compression);
        } else {
            options->setOptions(job.repository, job.encrypted, job.format, job.compression,
                                job.tarIncremental);
            if (job.encrypted) {
                std::string secret, detail;
                if (CredentialStore::load(("job/" + job.id).toStdString(), secret, detail))
                    options->setPassword(QString::fromUtf8(secret.c_str()));
            }
        }
        auto* runModes = new QButtonGroup(dialog);
        auto* scheduled = new QRadioButton(tr("定时"));
        scheduled->setObjectName("planScheduled");
        auto* realtime = new QRadioButton(tr("实时"));
        realtime->setObjectName("planRealtime");
        runModes->addButton(scheduled);
        runModes->addButton(realtime);
        if (job.scheduled != job.realtime) {
            scheduled->setChecked(job.scheduled);
            realtime->setChecked(job.realtime);
        }
        auto* modeRow = new QHBoxLayout;
        modeRow->addWidget(scheduled);
        modeRow->addWidget(realtime);
        if (!isNew && job.scheduled && job.realtime) {
            auto* legacy = new QLabel(tr("原计划同时启用两种方式，请选择一种"));
            legacy->setWordWrap(true);
            modeRow->addWidget(legacy);
        }
        if (!isNew && !job.scheduled && !job.realtime) {
            auto* manual = new QRadioButton(tr("手动（已有计划）"));
            runModes->addButton(manual);
            modeRow->addWidget(manual);
            manual->setChecked(true);
        }
        modeRow->addStretch();
        auto* cron = new QLineEdit(job.cron.isEmpty() ? QStringLiteral("0 2 * * *") : job.cron);
        cron->setObjectName("planCron");
        cron->setEnabled(scheduled->isChecked());
        connect(scheduled, &QRadioButton::toggled, cron, &QWidget::setEnabled);
        options->setRepositoryRequired(realtime->isChecked());
        connect(realtime, &QRadioButton::toggled, options, &BackupOptionsWidget::setRepositoryRequired);
        auto* retention = new QSpinBox;
        retention->setRange(1, 999);
        retention->setValue(job.retention);
        auto filter = std::make_shared<FilterOptions>(isNew ? filters_ : job.filter);
        auto* filterButton = button(tr("筛选配置…"));
        connect(filterButton, &QPushButton::clicked, dialog, [dialog, filter] {
            FilterDialog editor(*filter, dialog);
            if (editor.exec() == QDialog::Accepted)
                *filter = editor.options();
        });
        auto* previewButton = button(tr("扫描预览"));
        connect(previewButton, &QPushButton::clicked, dialog, [dialog, model, filter] {
            std::vector<std::string> sources;
            for (const auto& source : model->roots())
                sources.push_back(source.toUtf8().toStdString());
            FilterOptions selected = *filter;
            selected.sourceRules = model->rules();
            BackupPreview preview;
            std::string error;
            if (!ArchiveService::preview(sources, selected, preview, error, true)) {
                QMessageBox::warning(dialog, QObject::tr("预览失败"),
                                     QString::fromUtf8(error.c_str()));
                return;
            }
            QMessageBox::information(dialog, QObject::tr("扫描预览"),
                                     previewDetails(preview, error));
        });
        form->addRow(tr("运行"), modeRow);
        form->addRow(tr("名称"), name);
        form->addRow(tr("保存目录"), targetRow);
        form->addRow(options);
        auto* cronRow = new QHBoxLayout;
        cronRow->addWidget(cron);
        auto* cronHelp = new QLabel(tr("?"), dialog);
        cronHelp->setObjectName("cronHelp");
        cronHelp->setToolTip(tr("五个字段：分钟 小时 日期 月份 星期。\n"
                                "* 表示任意；支持逗号、范围和步长，例如 */15。\n"
                                "例：0 2 * * * 表示每天 02:00；0 9 * * 1-5 表示工作日 09:00。\n"
                                "星期 0 或 7 表示周日；日期与星期均受限时满足任一即可。"));
        cronHelp->setAccessibleName(tr("Cron 用法"));
        cronHelp->setCursor(Qt::WhatsThisCursor);
        cronRow->addWidget(cronHelp);
        form->addRow(tr("Cron"), cronRow);
        form->addRow(tr("保留数量"), retention);
        auto* filterRow = new QHBoxLayout;
        filterRow->addWidget(filterButton);
        filterRow->addWidget(previewButton);
        form->addRow(filterRow);
        body->addWidget(formWidget, 3);
        outer->addLayout(body, 1);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, job, isNew, filter, options, name, target, model, runModes, scheduled, realtime, cron, retention, refresh]() mutable {
            if (!runModes->checkedButton()) {
                QMessageBox::warning(dialog, tr("请选择运行方式"), tr("请选择定时或实时。"));
                return;
            }
            if (!isNew) {
                bool found = false;
                for (const auto& current : jobs_->jobs()) {
                    if (current.id != job.id)
                        continue;
                    job.enabled = current.enabled;
                    job.lastRun = current.lastRun;
                    job.lastSuccess = current.lastSuccess;
                    job.nextRun = current.nextRun;
                    job.lastResult = current.lastResult;
                    job.realtimeStatus = current.realtimeStatus;
                    job.realtimeTriggered = current.realtimeTriggered;
                    job.fullRescan = current.fullRescan;
                    job.changedPaths = current.changedPaths;
                    found = true;
                    break;
                }
                if (!found) {
                    QMessageBox::warning(dialog, tr("无法保存计划"), tr("该计划已被删除。"));
                    return;
                }
            }
            const auto pack = options->options();
            job.name = name->text();
            job.destination = target->text();
            job.sources = model->roots();
            job.filter = *filter;
            job.filter.sourceRules = model->rules();
            job.repository = options->incremental();
            job.tarIncremental = options->tarIncremental();
            job.encrypted = options->encryptionEnabled();
            job.format = pack.format;
            job.compression = pack.compression;
            job.scheduled = scheduled->isChecked();
            job.realtime = realtime->isChecked();
            job.cron = cron->text();
            job.retention = retention->value();
            if (job.id.isEmpty())
                job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const std::string key = ("job/" + job.id).toStdString();
            std::string oldPassword, detail;
            bool hadOld = false, changed = false;
            if (job.encrypted) {
                hadOld = CredentialStore::load(key, oldPassword, detail);
                detail.clear();
                if (!pack.password.empty()) {
                    if (!CredentialStore::save(key, pack.password, detail)) {
                        QMessageBox::warning(dialog, tr("凭据保存失败"),
                                             QString::fromStdString(detail));
                        return;
                    }
                    changed = true;
                } else if (!hadOld) {
                    QMessageBox::warning(dialog, tr("需要密码"),
                                         tr("请输入密码并确保密钥环可用。"));
                    return;
                }
            }
            QString error;
            if (!jobs_->upsert(job, error)) {
                if (changed) {
                    std::string rollback;
                    const bool restored = hadOld ? CredentialStore::save(key, oldPassword, rollback)
                                                 : CredentialStore::remove(key, rollback);
                    if (!restored)
                        error += tr("\n凭据回滚失败：%1").arg(QString::fromStdString(rollback));
                }
                QMessageBox::warning(dialog, tr("无法保存计划"), error);
                return;
            }
            dialog->accept();
            refresh();
        });
        dialog->show();
    };
    connect(add, &QPushButton::clicked, this, [configure] {
        BackupJob job;
        configure(job, true);
    });
    connect(edit, &QPushButton::clicked, this, [selected, configure] {
        const auto job = selected();
        if (!job.id.isEmpty())
            configure(job, false);
    });
    connect(run, &QPushButton::clicked, this, [this, selected] {
        const auto job = selected();
        if (job.id.isEmpty())
            return;
        QString error;
        if (!jobs_->runNow(job.id, error))
            QMessageBox::warning(this, tr("无法运行计划"), error);
        else if (!error.isEmpty())
            appendLog(error);
    });
    connect(toggle, &QPushButton::clicked, this, [this, selected, refresh] {
        const auto job = selected();
        if (job.id.isEmpty())
            return;
        QString error;
        if (!jobs_->setEnabled(job.id, !job.enabled, error))
            QMessageBox::warning(this, tr("无法更新计划"), error);
        refresh();
    });
    connect(remove, &QPushButton::clicked, this, [this, selected, refresh] {
        const auto job = selected();
        if (job.id.isEmpty())
            return;
        QString error;
        if (!jobs_->remove(job.id, error)) {
            QMessageBox::warning(this, tr("无法删除计划"), error);
            return;
        }
        std::string detail;
        if (!CredentialStore::remove(("job/" + job.id).toStdString(), detail))
            appendLog(tr("无法删除计划凭据：%1").arg(QString::fromStdString(detail)), true);
        refresh();
    });
    connect(details, &QPushButton::clicked, this, [this, selected] {
        const auto job = selected();
        if (job.id.isEmpty() || busy_) return;
        if (planDetails_) delete planDetails_.data();
        auto* dialog = new QDialog(this);
        dialog->setObjectName("planDetails");
        planDetails_ = dialog;
        dialog->setWindowTitle(tr("计划详情 · %1").arg(job.name));
        dialog->resize(850, 560);
        auto* layout = new QVBoxLayout(dialog);
        layout->addWidget(createHistoryPage(job), 1);
        dialog->show();
    });
    refresh();
    return page;
}

QWidget* MainWindow::createHistoryPage(const BackupJob& job) {
    QVBoxLayout* layout = nullptr;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 6, 14, 6);
    layout->setSpacing(5);
    auto* heading = new QHBoxLayout;
    auto* titleLabel = new QLabel(tr("历史版本"));
    titleLabel->setObjectName("pageTitle");
    heading->addWidget(titleLabel);
    heading->addStretch();
    auto *path = new QLineEdit, *password = new QLineEdit;
    auto* historyType = new QComboBox;
    historyType->addItem(tr("版本仓库"));
    historyType->addItem(tr("增量归档"));
    historyType->addItem(tr("独立归档"));
    historyType->setCurrentIndex(job.tarIncremental ? 1 : (job.repository ? 0 : 2));
    path->setText(job.tarIncremental ? QDir(job.destination).filePath("dbackup-" + job.id + "-tar-incremental") : job.destination);
    path->setPlaceholderText(tr("仓库路径"));
    password->setPlaceholderText(tr("输入恢复密码"));
    password->setObjectName("planHistoryPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaximumWidth(200);
    auto* passwordLabel = new QLabel(tr("密码"));
    passwordLabel->setObjectName("planHistoryPasswordLabel");
    heading->addWidget(passwordLabel);
    heading->addWidget(password);
    layout->addLayout(heading);
    scroll->setWidget(page);
    if (job.encrypted) {
        std::string secret, detail;
        if (CredentialStore::load(("job/" + job.id).toStdString(), secret, detail))
            password->setText(QString::fromUtf8(secret.c_str()));
    }
    auto *browse = button(tr("选择仓库…")), *load = button(tr("刷新"), true);
    auto* row = new QHBoxLayout;
    row->addWidget(historyType);
    row->addWidget(path, 1);
    row->addWidget(browse);
    row->addWidget(load);
    historyType->hide();
    path->hide();
    browse->hide();
    load->hide();
    layout->addLayout(row);
    auto* historyStatus = new QLabel;
    historyStatus->setObjectName("planHistoryStatus");
    historyStatus->setWordWrap(true);
    historyStatus->setProperty("muted", true);
    layout->addWidget(historyStatus);
    auto* table = new QTableWidget(0, 3);
    table->setAlternatingRowColors(true);
    table->setObjectName("planHistoryTable");
    load->setObjectName("loadPlanHistory");
    auto* restore = button(tr("恢复所选快照"), true);
    table->setHorizontalHeaderLabels({tr("快照 ID"), tr("创建时间"), tr("变更文件")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    connect(path, &QLineEdit::textChanged, table, [table] { table->setRowCount(0); });
    const auto setHistoryEnabled = [historyType, path, password, browse, load, restore, table](bool enabled) {
        for (auto* widget : {static_cast<QWidget*>(historyType), static_cast<QWidget*>(path),
                             static_cast<QWidget*>(password), static_cast<QWidget*>(browse),
                             static_cast<QWidget*>(load), static_cast<QWidget*>(restore),
                             static_cast<QWidget*>(table)})
            widget->setEnabled(enabled);
    };
    struct ChainTask {
        std::atomic_bool cancelled{false};
        bool ok = false;
        std::string error;
        std::vector<dbackup::incremental::VersionInfo> versions;
    };
    connect(browse, &QPushButton::clicked, this, [this, path, historyType] {
        QString p = QFileDialog::getExistingDirectory(this, historyType->currentIndex() == 1
                                                              ? tr("选择增量归档目录")
                                                              : tr("选择仓库"));
        if (!p.isEmpty())
            path->setText(p);
    });
    connect(historyType, &QComboBox::currentIndexChanged, this, [table, path](int index) {
        table->setRowCount(0);
        path->setPlaceholderText(index == 1 ? QObject::tr("增量归档目录")
                                            : QObject::tr("仓库路径"));
    });
    connect(load, &QPushButton::clicked, this, [this, path, password, table, historyType, setHistoryEnabled, job, historyStatus] {
        if (!QFileInfo::exists(path->text())) {
            table->setRowCount(0);
            historyStatus->setText(tr("暂无备份记录。"));
            return;
        }
        historyStatus->clear();
        if (historyType->currentIndex() == 2) {
            table->setRowCount(0);
            const QString prefix = "dbackup-" + job.id + "-";
            const QRegularExpression managed(QStringLiteral("^%1[0-9]{8}-[0-9]{6}-[0-9]{3}-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(tar|abk)$").arg(QRegularExpression::escape(prefix)));
            for (const auto& file : QDir(path->text()).entryInfoList({prefix + "*.tar", prefix + "*.abk"}, QDir::Files | QDir::NoSymLinks, QDir::Time)) {
                if (!managed.match(file.fileName()).hasMatch()) continue;
                const int row = table->rowCount();
                table->insertRow(row);
                auto* item = new QTableWidgetItem(file.fileName());
                item->setData(Qt::UserRole, file.absoluteFilePath());
                table->setItem(row, 0, item);
                table->setItem(row, 1, new QTableWidgetItem(file.lastModified().toString("yyyy-MM-dd HH:mm:ss")));
                table->setItem(row, 2, new QTableWidgetItem(tr("变化分类不可用")));
            }
            return;
        }
        if (historyType->currentIndex() == 1) {
            if (busy_)
                return;
            const auto directory = path->text().toUtf8().toStdString();
            auto task = std::make_shared<ChainTask>();
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(cancelButton_, &QPushButton::clicked, this,
                                  [task] { task->cancelled.store(true); });
            cancelled_ = false;
            setBusy(true, tr("正在读取备份历史…"));
            setHistoryEnabled(false);
            QThread* worker = QThread::create([task, directory] {
                OperationContext context;
                context.cancelled = &task->cancelled;
                task->ok = dbackup::incremental::listPublicVersions(directory, context,
                                                                    task->versions, task->error);
            });
            connect(worker, &QThread::finished, this, [this, table, task, connection, setHistoryEnabled, historyStatus] {
                disconnect(*connection);
                setHistoryEnabled(true);
                setBusy(false);
                if (!task->ok) {
                    historyStatus->setText(tr("读取失败，请检查计划密码和备份目录。"));
                    QMessageBox::warning(this, tr("读取失败"), QString::fromUtf8(task->error.c_str()));
                    return;
                }
                table->setRowCount(0);
                for (const auto& version : task->versions) {
                    const int row = table->rowCount();
                    table->insertRow(row);
                    auto* id = new QTableWidgetItem(version.sequence
                        ? QString::number(static_cast<qulonglong>(version.sequence))
                        : QString::fromUtf8(version.directoryName.c_str()));
                    id->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(version.sequence));
                    id->setData(Qt::UserRole + 1, QString::fromUtf8(version.directoryName.c_str()));
                    table->setItem(row, 0, id);
                    table->setItem(row, 1, new QTableWidgetItem(
                        version.createdAtMs ? QDateTime::fromMSecsSinceEpoch(version.createdAtMs).toString("yyyy-MM-dd HH:mm:ss")
                                            : tr("—")));
                    const QString changes = version.changesAvailable
                        ? changeSummary(version.added, version.modified, version.deleted)
                        : tr("旧备份没有公开变化摘要");
                    auto* changeItem = new QTableWidgetItem(changes);
                    changeItem->setToolTip(changes);
                    table->setItem(row, 2, changeItem);
                }
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
            return;
        }
        LocalRepository repo(path->text().toUtf8().toStdString());
        std::vector<SnapshotInfo> all;
        std::string error;
        if (!repo.listSnapshots(all, error)) {
            historyStatus->setText(tr("读取失败，请检查计划密码和备份目录。"));
            QMessageBox::warning(this, tr("读取失败"), QString::fromUtf8(error.c_str()));
            return;
        }
        table->setRowCount(0);
        for (auto& s : all) {
            int r = table->rowCount();
            table->insertRow(r);
            table->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(s.id)));
            table->setItem(
                r, 1,
                new QTableWidgetItem(
                    QDateTime::fromMSecsSinceEpoch(s.createdAtMs).toString("yyyy-MM-dd HH:mm:ss")));
            auto* state = new QTableWidgetItem(s.partial ? tr("部分完成，变化分类不可用") : tr("变化分类不可用"));
            if (!s.warnings.empty())
                state->setToolTip(QString::fromUtf8(s.warnings.front().c_str()));
            table->setItem(r, 2, state);
        }
    });
    QTimer::singleShot(0, page, [load] { load->click(); });
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    actions->addWidget(restore);
    layout->addLayout(actions);
    connect(restore, &QPushButton::clicked, this, [this, path, password, table, historyType, setHistoryEnabled, job] {
        if (table->currentRow() < 0)
            return;
        if (historyType->currentIndex() == 2) {
            const QString archive = table->item(table->currentRow(), 0)->data(Qt::UserRole).toString();
            const QString dest = QFileDialog::getExistingDirectory(this, tr("恢复到"));
            if (dest.isEmpty()) return;
            archivePath_->setText(archive);
            restoreTarget_->setText(dest);
            restorePassword_->setText(password->text());
            startRestore();
            return;
        }
        if (historyType->currentIndex() == 1) {
            if (busy_)
                return;
            const QString parent = QFileDialog::getExistingDirectory(this, tr("选择恢复目录的上级目录"));
            if (parent.isEmpty())
                return;
            bool accepted = false;
            const QString name = QInputDialog::getText(this, tr("恢复到新目录"), tr("新目录名称"),
                                                       QLineEdit::Normal, tr("DBackup-恢复"), &accepted).trimmed();
            if (!accepted)
                return;
            if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\')) {
                QMessageBox::warning(this, tr("目录名称无效"), tr("请输入不含路径分隔符的目录名称。"));
                return;
            }
            const auto versionDirectory = table->item(table->currentRow(), 0)->data(Qt::UserRole + 1).toString().toUtf8().toStdString();
            const QString dest = QDir(parent).filePath(name);
            const auto directory = path->text().toUtf8().toStdString();
            const auto secret = password->text().toUtf8().toStdString();
            const auto target = dest.toUtf8().toStdString();
            auto task = std::make_shared<ChainTask>();
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(cancelButton_, &QPushButton::clicked, this,
                                  [task] { task->cancelled.store(true); });
            cancelled_ = false;
            setBusy(true, tr("正在恢复增量备份…"));
            setHistoryEnabled(false);
            QThread* worker = QThread::create([task, directory, secret, versionDirectory, target] {
                OperationContext context;
                context.cancelled = &task->cancelled;
                std::vector<dbackup::incremental::VersionInfo> verified;
                if (!dbackup::incremental::listVersions(directory, secret, context, verified, task->error)) return;
                const auto item = std::find_if(verified.begin(), verified.end(), [&](const auto& v) {
                    return v.directoryName == versionDirectory;
                });
                if (item == verified.end()) { task->error = "选中的备份版本已不存在"; return; }
                task->ok = dbackup::incremental::restoreVersion(directory, item->sequence, secret,
                                                                 target, context, task->error);
            });
            connect(worker, &QThread::finished, this, [this, task, dest, connection, setHistoryEnabled] {
                disconnect(*connection);
                setHistoryEnabled(true);
                setBusy(false);
                if (!task->ok)
                    QMessageBox::critical(this, tr("恢复失败"), QString::fromUtf8(task->error.c_str()));
                else if (!task->error.empty())
                    QMessageBox::warning(this, tr("恢复完成（有提示）"), QString::fromUtf8(task->error.c_str()));
                else
                    QMessageBox::information(this, tr("恢复完成"), tr("备份已恢复到：\n%1").arg(dest));
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
            return;
        }
        QString dest = QFileDialog::getExistingDirectory(this, tr("恢复到"));
        if (dest.isEmpty())
            return;
        LocalRepository repo(path->text().toUtf8().toStdString());
        std::string error;
        OperationContext context;
        bool ok = repo.restoreSnapshot(table->item(table->currentRow(), 0)->text().toStdString(),
                                       dest.toUtf8().toStdString(),
                                       password->text().toUtf8().toStdString(), error, context);
        if (!ok)
            QMessageBox::critical(this, tr("恢复失败"), QString::fromUtf8(error.c_str()));
        else if (!error.empty())
            QMessageBox::warning(this, tr("恢复完成（部分元数据未恢复）"),
                                 QString::fromUtf8(error.c_str()));
        else
            QMessageBox::information(this, tr("恢复完成"), tr("快照已恢复"));
    });
    return scroll;
}

QWidget* MainWindow::createSettingsPage() {
    QVBoxLayout* layout = nullptr;
    QWidget* page = createPageShell(tr("设置"), &layout);
    auto* group = new QGroupBox(tr("系统集成"));
    auto* box = new QVBoxLayout(group);
    auto* startup = new QCheckBox(tr("登录桌面时自动启动 DBackup"));
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
    const QString path = directory + "/dbackup.desktop";
    startup->setChecked(QFile::exists(path));
    connect(startup, &QCheckBox::toggled, this, [this, startup, directory, path](bool enabled) {
        bool ok = true;
        if (enabled) {
            ok = QDir().mkpath(directory);
            QString executable = QCoreApplication::applicationFilePath();
            executable.replace("\\", "\\\\")
                .replace("\"", "\\\"")
                .replace("`", "\\`")
                .replace("$", "\\$")
                .replace("%", "%%");
            QByteArray content =
                (QString("[Desktop Entry]\nType=Application\nName=DBackup\nExec=\"") + executable +
                 "\"\nTerminal=false\n")
                    .toUtf8();
            QSaveFile file(path);
            ok = ok && file.open(QIODevice::WriteOnly) && file.write(content) == content.size() &&
                 file.commit();
        } else if (QFile::exists(path))
            ok = QFile::remove(path);
        if (!ok) {
            startup->blockSignals(true);
            startup->setChecked(!enabled);
            startup->blockSignals(false);
            QMessageBox::warning(this, tr("设置失败"), tr("无法更新桌面自动启动设置。"));
        }
    });
    box->addWidget(startup);
    auto* tray = new QLabel(tr("关闭窗口后在托盘运行"));
    tray->setWordWrap(true);
    tray->setProperty("muted", true);
    box->addWidget(tray);
    layout->addWidget(group);
    layout->addStretch();
    return page;
}

void MainWindow::updateSourceSummary() {
    sourceSummary_->setText(tr("已选择 %1 个来源").arg(sourceModel_->roots().size()));
    previewSummary_->setText(tr("选择已更改，请重新扫描"));
}
void MainWindow::chooseBackupTarget() {
    const PackOptions options = backupOptions_->options();
    bool plainTar = options.format == PackFormat::Tar && options.compression == Compression::None &&
                    !backupOptions_->encryptionEnabled();
    QString suffix = plainTar ? ".tar" : ".abk";
    QString path = QFileDialog::getSaveFileName(this, tr("保存备份归档"), {},
                                                plainTar ? tr("Tar 归档 (*.tar)")
                                                         : tr("DBackup 归档 (*.abk)"));
    if (!path.isEmpty()) {
        if (!path.endsWith(suffix, Qt::CaseInsensitive))
            path += suffix;
        backupTarget_->setText(path);
    }
}
void MainWindow::chooseArchive() {
    const QString path = QFileDialog::getOpenFileName(this, tr("选择备份归档"), {},
                                                      tr("备份归档 (*.abk *.tar);;所有文件 (*)"));
    if (!path.isEmpty())
        archivePath_->setText(path);
}
void MainWindow::chooseRestoreFolder() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择恢复目录"));
    if (!path.isEmpty())
        restoreTarget_->setText(path);
}

void MainWindow::editFilters() {
    FilterDialog dialog(filters_, this);
    if (dialog.exec() == QDialog::Accepted) {
        filters_ = dialog.options();
        updateFilterSummary();
        previewSummary_->setText(tr("规则已更改，请重新扫描预览"));
    }
}
void MainWindow::updateFilterSummary() {
    QStringList parts;
    if (!filters_.extensions.empty())
        parts << tr("%1 种扩展名").arg(filters_.extensions.size());
    if (!filters_.pathContains.empty())
        parts << tr("路径包含“%1”").arg(QString::fromUtf8(filters_.pathContains.c_str()));
    if (!filters_.nameContains.empty())
        parts << tr("名称包含“%1”").arg(QString::fromUtf8(filters_.nameContains.c_str()));
    if (filters_.minSize)
        parts << tr("≥ %1").arg(humanSize(filters_.minSize));
    if (filters_.maxSize)
        parts << tr("≤ %1").arg(humanSize(filters_.maxSize));
    if (!filters_.pathRules.empty())
        parts << tr("%1 条包含/排除规则").arg(filters_.pathRules.size());
    filterSummary_->setText(parts.isEmpty() ? tr("未设置筛选：将包含所有可读取的项目")
                                            : parts.join(tr(" · ")));
}

std::vector<std::string> MainWindow::sourcePaths() const {
    std::vector<std::string> paths;
    for (const auto& path : sourceModel_->roots())
        paths.push_back(path.toUtf8().toStdString());
    return paths;
}

void MainWindow::previewFilters() {
    if (busy_)
        return;
    const auto sources = sourcePaths();
    if (sources.empty()) {
        QMessageBox::information(this, tr("扫描预览"), tr("请先添加至少一个备份来源。"));
        return;
    }
    setBusy(true, tr("正在扫描文件元数据…"));
    FilterOptions filters = filters_;
    filters.sourceRules = sourceModel_->rules();
    QThread* worker = QThread::create([this, sources, filters] {
        BackupPreview preview;
        std::string error;
        const bool ok = ArchiveService::preview(sources, filters, preview, error, true);
        QMetaObject::invokeMethod(
            this,
            [this, ok, preview, error] {
                setBusy(false);
                if (!ok) {
                    QMessageBox::warning(this, tr("预览失败"), QString::fromUtf8(error.c_str()));
                    appendLog(tr("扫描预览失败：%1").arg(QString::fromUtf8(error.c_str())), true);
                    return;
                }
                previewSummary_->setText(
                    tr("预计包含 %1 个文件、%2 个文件夹，共 %3；排除 %4 个文件")
                        .arg(preview.includedFiles)
                        .arg(preview.includedDirectories)
                        .arg(humanSize(preview.includedBytes))
                        .arg(preview.excludedFiles));
                appendLog(tr("扫描预览完成：%1 个文件，%2")
                              .arg(preview.includedFiles)
                              .arg(humanSize(preview.includedBytes)));
                if (!error.empty())
                    appendLog(tr("部分来源无法读取：%1").arg(QString::fromUtf8(error.c_str())),
                              true);
                QMessageBox::information(this, tr("扫描预览"), previewDetails(preview, error));
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::startBackup() {
    if (busy_)
        return;
    const auto sources = sourcePaths();
    const QString target = backupTarget_->text().trimmed();
    if (sources.empty() || target.isEmpty()) {
        QMessageBox::information(this, tr("无法开始备份"),
                                 tr("请添加备份来源并选择归档保存位置。"));
        return;
    }
    PackOptions options = backupOptions_->options();
    options.filter = filters_;
    options.filter.sourceRules = sourceModel_->rules();
    {
        const bool plainTar = options.format == PackFormat::Tar &&
                              options.compression == Compression::None &&
                              !backupOptions_->encryptionEnabled();
        if (plainTar && target.endsWith(".abk", Qt::CaseInsensitive)) {
            QMessageBox::warning(this, tr("文件名与格式不一致"),
                                 tr("当前输出为标准 tar，请重新选择 .tar 保存路径。"));
            return;
        }
        if (!plainTar && target.endsWith(".tar", Qt::CaseInsensitive)) {
            QMessageBox::warning(this, tr("文件名与格式不一致"),
                                 tr("当前输出为 DBackup 封装（.abk）；tar "
                                    "只是不压缩、不加密时的外部格式。请重新选择保存路径。"));
            return;
        }
    }
    if (backupOptions_->encryptionEnabled() && options.password.empty()) {
        QMessageBox::information(this, tr("需要加密密码"), tr("使用 AES 加密时请输入密码。"));
        return;
    }
    cancelled_ = false;
    setBusy(true, tr("正在创建备份归档…"));
    appendLog(tr("开始备份到 %1").arg(target));
    QThread* worker = QThread::create([this, sources, target, options] {
        OperationContext context;
        context.cancelled = &cancelled_;
        context.includeSourceDirectory = true;
        context.onProgress = [this](const OperationProgress& p) {
            QMetaObject::invokeMethod(this, [this, p] { updateProgress(p); }, Qt::QueuedConnection);
        };
        std::string error;
        const bool ok =
            ArchiveService::create(sources, target.toUtf8().toStdString(), options, error, context);
        std::string digest, digestError;
        if (ok)
            dbackup::crypto::md5File(target.toUtf8().toStdString(), digest, digestError);
        QMetaObject::invokeMethod(
            this,
            [this, ok, target, error, digest, digestError] {
                setBusy(false);
                if (ok) {
                    const bool partial = !error.empty();
                    appendLog(partial ? tr("备份部分完成：%1").arg(target)
                                      : tr("备份完成：%1").arg(target),
                              partial);
                    QString message = tr("备份已写入：\n%1").arg(target);
                    if (!digest.empty())
                        message +=
                            tr("\nMD5（仅供摘要展示）：%1").arg(QString::fromStdString(digest));
                    else if (!digestError.empty())
                        message +=
                            tr("\nMD5 计算失败：%1").arg(QString::fromStdString(digestError));
                    if (partial) {
                        appendLog(QString::fromUtf8(error.c_str()), true);
                        message += tr("\n\n跳过的项目：\n%1").arg(QString::fromUtf8(error.c_str()));
                        QMessageBox::warning(this, tr("备份部分完成"), message);
                    } else
                        QMessageBox::information(this, tr("备份完成"), message);
                } else {
                    appendLog(tr("备份失败：%1").arg(QString::fromUtf8(error.c_str())), true);
                    QMessageBox::critical(this, tr("备份失败"), QString::fromUtf8(error.c_str()));
                }
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::startRestore() {
    if (busy_)
        return;
    const QString archive = archivePath_->text().trimmed(),
                  target = restoreTarget_->text().trimmed(), password = restorePassword_->text();
    if (archive.isEmpty() || target.isEmpty()) {
        QMessageBox::information(this, tr("无法开始恢复"), tr("请选择归档文件和恢复目录。"));
        return;
    }
    cancelled_ = false;
    setBusy(true, tr("正在恢复文件…"));
    appendLog(tr("开始从 %1 恢复").arg(archive));
    QThread* worker = QThread::create([this, archive, target, password] {
        OperationContext context;
        context.cancelled = &cancelled_;
        context.onProgress = [this](const OperationProgress& p) {
            QMetaObject::invokeMethod(this, [this, p] { updateProgress(p); }, Qt::QueuedConnection);
        };
        std::string error;
        const bool ok =
            ArchiveService::extract(archive.toUtf8().toStdString(), target.toUtf8().toStdString(),
                                    error, password.toUtf8().toStdString(), context);
        QMetaObject::invokeMethod(
            this,
            [this, ok, target, error] {
                setBusy(false);
                if (ok && !error.empty()) {
                    const QString warning = QString::fromUtf8(error.c_str());
                    appendLog(tr("恢复完成，有跳过项或提示：%1\n%2").arg(target, warning), true);
                    QMessageBox::warning(this, tr("恢复完成（有提示）"), warning);
                } else if (ok) {
                    appendLog(tr("恢复完成：%1").arg(target));
                    QMessageBox::information(this, tr("恢复完成"),
                                             tr("文件已恢复到：\n%1").arg(target));
                } else {
                    appendLog(tr("恢复失败：%1").arg(QString::fromUtf8(error.c_str())), true);
                    QMessageBox::critical(this, tr("恢复失败"), QString::fromUtf8(error.c_str()));
                }
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::updateProgress(const OperationProgress& p) {
    int percent = 0;
    if (p.totalBytes)
        percent = static_cast<int>(100 * p.completedBytes / p.totalBytes);
    else if (p.totalFiles)
        percent = static_cast<int>(100 * p.completedFiles / p.totalFiles);
    progress_->setValue(qBound(0, percent, 100));
    QStringList details{stageName(p.stage)};
    if (p.totalFiles)
        details << tr("%1/%2 个项目").arg(p.completedFiles).arg(p.totalFiles);
    if (p.totalBytes)
        details << tr("%1/%2").arg(humanSize(p.completedBytes), humanSize(p.totalBytes));
    if (p.bytesPerSecond > 0) {
        details << tr("%1/s").arg(humanSize(static_cast<uint64_t>(p.bytesPerSecond)));
        if (p.totalBytes > p.completedBytes)
            details << tr("剩余约 %1 秒")
                           .arg(static_cast<qulonglong>((p.totalBytes - p.completedBytes) /
                                                        p.bytesPerSecond));
    }
    progressDetails_->setText(details.join(QStringLiteral(" · ")));
    if (!p.currentPath.empty())
        statusBar()->showMessage(QString::fromUtf8(p.currentPath.c_str()));
}

void MainWindow::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    backupButton_->setEnabled(!busy);
    restoreButton_->setEnabled(!busy);
    cancelButton_->setVisible(busy);
    if (busy) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progressDetails_->setText(message);
        statusBar()->showMessage(message);
    } else {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progressDetails_->setText(tr("就绪"));
        statusBar()->showMessage(tr("就绪"));
    }
}
void MainWindow::appendLog(const QString& message, bool error) {
    activityLog_->appendPlainText(QString("[%1] %2%3")
                                      .arg(QDateTime::currentDateTime().toString("HH:mm:ss"),
                                           error ? tr("错误：") : QString(), message));
}
