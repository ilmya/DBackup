#include "gui/main_window.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QSettings>
#include <QTableWidget>
#include <QStatusBar>
#include <QThread>
#include <QVBoxLayout>

#include "gui/filter_dialog.h"
#include "automation/job_manager.h"
#include "repository/repository.h"
#include "security/credential_store.h"
#include "network/remote_client.h"
#include "network/remote_repository_service.h"

namespace {
QPushButton *button(const QString &text, bool primary = false) {
    auto *result = new QPushButton(text);
    if (primary) result->setProperty("primary", true);
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QString humanSize(uint64_t bytes) {
    static const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) { value /= 1024.0; ++unit; }
    return QString::number(value, 'f', unit == 0 ? 0 : 2) + " " + units[unit];
}

QString stageName(OperationStage stage) {
    switch (stage) {
    case OperationStage::Scanning: return QObject::tr("正在扫描");
    case OperationStage::Reading: return QObject::tr("正在读取");
    case OperationStage::Compressing: return QObject::tr("正在压缩");
    case OperationStage::Encrypting: return QObject::tr("正在加密");
    case OperationStage::Writing: return QObject::tr("正在写入");
    case OperationStage::Uploading: return QObject::tr("正在上传");
    case OperationStage::Downloading: return QObject::tr("正在下载");
    case OperationStage::Restoring: return QObject::tr("正在恢复");
    case OperationStage::Completed: return QObject::tr("已完成");
    }
    return {};
}

QFrame *metricCard(const QString &title, const QString &value, const QString &detail) {
    auto *card = new QFrame;
    card->setProperty("card", true);
    auto *layout = new QVBoxLayout(card);
    auto *titleLabel = new QLabel(title); titleLabel->setProperty("muted", true);
    auto *valueLabel = new QLabel(value); valueLabel->setProperty("metric", true);
    auto *detailLabel = new QLabel(detail); detailLabel->setProperty("muted", true);
    layout->addWidget(titleLabel); layout->addWidget(valueLabel); layout->addWidget(detailLabel);
    return card;
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("DBackup · 备份管理器"));
    resize(1180, 760);
    setMinimumSize(960, 650);
    auto *root = new QWidget; root->setObjectName("appRoot");
    auto *layout = new QHBoxLayout(root); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);

    auto *sidebar = new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(210);
    auto *navLayout = new QVBoxLayout(sidebar); navLayout->setContentsMargins(14, 22, 14, 18);
    auto *brand = new QLabel("DBackup"); brand->setObjectName("brand");
    auto *brandSub = new QLabel(tr("安全、清晰、可扩展的备份")); brandSub->setObjectName("brandSub");
    navLayout->addWidget(brand); navLayout->addWidget(brandSub);

    pages_ = new QStackedWidget;
    pages_->addWidget(createDashboardPage());
    pages_->addWidget(createBackupPage());
    pages_->addWidget(createRestorePage());
    jobs_ = new JobManager(this);remote_=new RemoteClient(this);remoteRepository_=new RemoteRepositoryService(remote_,this);
    remote_->setCertificatePrompt([this](const QString &server,const QString &fingerprint){
        return QMessageBox::question(this,tr("确认服务器证书"),
            tr("这是首次连接 %1。\n\n证书 SHA-256 指纹：\n%2\n\n请与服务器管理员提供的指纹核对，确认无误后再信任。").arg(server,fingerprint),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes;
    });
    pages_->addWidget(createSchedulePage());
    pages_->addWidget(createHistoryPage());
    pages_->addWidget(createStoragePage());
    pages_->addWidget(createSettingsPage());

    const QStringList names = {tr("概览"), tr("新建备份"), tr("恢复数据"), tr("任务计划"),
                               tr("备份历史"), tr("存储位置"), tr("设置")};
    QList<QPushButton *> navButtons;
    for (int i = 0; i < names.size(); ++i) {
        auto *nav = new QPushButton(names[i]); nav->setProperty("nav", true); nav->setCheckable(true);
        nav->setAutoExclusive(true); nav->setCursor(Qt::PointingHandCursor);
        connect(nav, &QPushButton::clicked, this, [this, i] { pages_->setCurrentIndex(i); });
        navLayout->addWidget(nav); navButtons << nav;
    }
    navButtons.first()->setChecked(true);
    navLayout->addStretch();
    auto *version = new QLabel(tr("DBackup 1.0")); version->setStyleSheet("color:#64748b;padding:8px;");
    navLayout->addWidget(version);
    layout->addWidget(sidebar); layout->addWidget(pages_, 1);
    setCentralWidget(root);

    activityLog_ = new QPlainTextEdit; activityLog_->setReadOnly(true); activityLog_->setMaximumBlockCount(500);
    activityLog_->setPlaceholderText(tr("任务活动会显示在这里")); activityLog_->setFixedHeight(105);
    auto *logDock = new QGroupBox(tr("活动日志")); auto *logLayout = new QVBoxLayout(logDock); logLayout->addWidget(activityLog_);
    auto *dock = new QDockWidget; dock->setWidget(logDock); dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    progressDetails_ = new QLabel(tr("就绪")); progressDetails_->setMinimumWidth(260);
    progress_ = new QProgressBar; progress_->setRange(0, 100); progress_->setValue(0); progress_->setFixedWidth(180); progress_->setTextVisible(true);
    cancelButton_=button(tr("取消"));cancelButton_->setVisible(false);connect(cancelButton_,&QPushButton::clicked,this,[this]{cancelled_=true;statusBar()->showMessage(tr("正在取消…"));});
    statusBar()->addPermanentWidget(progressDetails_,1);statusBar()->addPermanentWidget(cancelButton_);statusBar()->addPermanentWidget(progress_); statusBar()->showMessage(tr("就绪"));
    appendLog(tr("DBackup 已启动，核心备份服务就绪。"));
    auto *trayMenu=new QMenu(this);trayMenu->addAction(tr("打开 DBackup"),this,[this]{showNormal();raise();activateWindow();});trayMenu->addAction(tr("暂停自动任务"),this,[this]{jobs_->pause(true);});trayMenu->addAction(tr("继续自动任务"),this,[this]{jobs_->pause(false);});trayMenu->addSeparator();trayMenu->addAction(tr("退出"),this,[this]{quitting_=true;qApp->quit();});
    tray_=new QSystemTrayIcon(windowIcon(),this);tray_->setToolTip("DBackup");tray_->setContextMenu(trayMenu);tray_->show();connect(tray_,&QSystemTrayIcon::activated,this,[this](auto reason){if(reason==QSystemTrayIcon::Trigger){showNormal();raise();}});
    jobs_->setExecutor([this](const BackupJob&job){QThread *worker=QThread::create([this,job]{std::string password,error;CredentialStore::load(("job/"+job.id).toStdString(),password,error);SnapshotOptions options;for(auto&s:job.sources)options.sources.push_back(s.toUtf8().toStdString());options.password=password;options.filter=job.filter;LocalRepository repo(job.destination.toUtf8().toStdString());SnapshotInfo info;bool ok=repo.createSnapshot(options,info,error);if(ok)repo.prune(job.retention,error);QMetaObject::invokeMethod(this,[this,job,ok,error]{jobs_->markFinished(job.id,ok,QString::fromUtf8(error.c_str()));appendLog(ok?tr("自动任务“%1”完成").arg(job.name):tr("自动任务“%1”失败：%2").arg(job.name,QString::fromUtf8(error.c_str())),!ok);},Qt::QueuedConnection);});connect(worker,&QThread::finished,worker,&QObject::deleteLater);worker->start();});
    jobs_->start();
}

void MainWindow::closeEvent(QCloseEvent *event){if(quitting_||!tray_->isVisible()){event->accept();return;}hide();tray_->showMessage(tr("DBackup 正在后台运行"),tr("定时和实时任务会继续执行，可从托盘菜单退出。"));event->ignore();}

QWidget *MainWindow::createPageShell(const QString &title, const QString &subtitle, QVBoxLayout **content) {
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(34, 28, 34, 28); layout->setSpacing(16);
    auto *titleLabel = new QLabel(title); titleLabel->setObjectName("pageTitle");
    auto *subtitleLabel = new QLabel(subtitle); subtitleLabel->setObjectName("pageSubtitle"); subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel); layout->addWidget(subtitleLabel); *content = layout; scroll->setWidget(page); return scroll;
}

QWidget *MainWindow::createDashboardPage() {
    QVBoxLayout *layout = nullptr; QWidget *page = createPageShell(tr("概览"), tr("从这里掌握备份状态，并快速开始常用操作。"), &layout);
    auto *metrics = new QHBoxLayout;
    metrics->addWidget(metricCard(tr("当前状态"), tr("就绪"), tr("等待创建备份任务")));
    metrics->addWidget(metricCard(tr("数据保护"), tr("AES-256-GCM"), tr("加密、认证与完整性校验")));
    metrics->addWidget(metricCard(tr("备份方式"), tr("完整 + 增量"), tr("便携归档与可恢复历史版本")));
    layout->addLayout(metrics);
    auto *quick = new QGroupBox(tr("快速开始")); auto *quickLayout = new QHBoxLayout(quick);
    auto *backup = button(tr("创建新备份"), true); auto *restore = button(tr("恢复已有归档"));
    connect(backup, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(1); });
    connect(restore, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(2); });
    quickLayout->addWidget(backup); quickLayout->addWidget(restore); quickLayout->addStretch(); layout->addWidget(quick);
    auto *notice = new QGroupBox(tr("保护建议")); auto *noticeLayout = new QVBoxLayout(notice);
    auto *text = new QLabel(tr("为重要数据同时保留本地和远程副本，并定期从备份历史中执行恢复检查。"));
    text->setWordWrap(true); text->setProperty("muted", true); noticeLayout->addWidget(text); layout->addWidget(notice); layout->addStretch();
    return page;
}

QWidget *MainWindow::createBackupPage() {
    QVBoxLayout *layout = nullptr; QWidget *page = createPageShell(tr("新建备份"), tr("选择多个文件或文件夹，设置输出、安全选项与筛选规则。"), &layout);
    auto *sources = new QGroupBox(tr("1. 备份来源")); auto *sourceLayout = new QVBoxLayout(sources);
    sourceList_ = new QListWidget; sourceList_->setMinimumHeight(120); sourceList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sourceLayout->addWidget(sourceList_);
    auto *sourceActions = new QHBoxLayout; auto *add = button(tr("添加来源"), true); auto *remove = button(tr("移除选中")); auto *clear = button(tr("清空"));
    auto *menu = new QMenu(add); auto *files = menu->addAction(tr("选择一个或多个文件…")); auto *folder = menu->addAction(tr("选择文件夹…")); add->setMenu(menu);
    connect(files, &QAction::triggered, this, [this] { addSources(false); }); connect(folder, &QAction::triggered, this, [this] { addSources(true); });
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeSelectedSources);
    connect(clear, &QPushButton::clicked, this, [this] { sourceList_->clear(); updateSourceSummary(); });
    sourceSummary_ = new QLabel(tr("0 个来源")); sourceSummary_->setProperty("muted", true);
    sourceActions->addWidget(add); sourceActions->addWidget(remove); sourceActions->addWidget(clear); sourceActions->addStretch(); sourceActions->addWidget(sourceSummary_);
    sourceLayout->addLayout(sourceActions); layout->addWidget(sources);

    auto *target = new QGroupBox(tr("2. 输出与安全")); auto *targetLayout = new QGridLayout(target);
    backupMode_=new QComboBox;backupMode_->addItems({tr("便携完整归档 (.abk)"),tr("版本化增量仓库")});
    backupTarget_ = new QLineEdit; backupTarget_->setPlaceholderText(tr("选择 .abk 归档保存位置")); auto *browse = button(tr("浏览…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::chooseBackupTarget);
    compression_ = new QSpinBox; compression_->setRange(0, 9); compression_->setValue(6); compression_->setToolTip(tr("0 为不压缩，9 为最高压缩率"));
    backupPassword_ = new QLineEdit; backupPassword_->setEchoMode(QLineEdit::Password); backupPassword_->setPlaceholderText(tr("可选；留空表示不加密"));
    auto *show = new QCheckBox(tr("显示密码")); connect(show, &QCheckBox::toggled, this, [this](bool checked) { backupPassword_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password); });
    targetLayout->addWidget(new QLabel(tr("备份模式")),0,0);targetLayout->addWidget(backupMode_,0,1);
    targetLayout->addWidget(new QLabel(tr("保存位置")), 1, 0); targetLayout->addWidget(backupTarget_, 1, 1); targetLayout->addWidget(browse, 1, 2);
    targetLayout->addWidget(new QLabel(tr("压缩等级")), 2, 0); targetLayout->addWidget(compression_, 2, 1);
    targetLayout->addWidget(new QLabel(tr("加密密码")), 3, 0); targetLayout->addWidget(backupPassword_, 3, 1); targetLayout->addWidget(show, 3, 2); layout->addWidget(target);

    auto *filter = new QGroupBox(tr("3. 自定义筛选")); auto *filterLayout = new QVBoxLayout(filter);
    filterSummary_ = new QLabel; filterSummary_->setWordWrap(true); filterSummary_->setProperty("muted", true);
    previewSummary_ = new QLabel(tr("尚未扫描预览")); previewSummary_->setProperty("muted", true);
    auto *filterActions = new QHBoxLayout; auto *edit = button(tr("配置筛选规则…")); auto *preview = button(tr("扫描预览"));
    connect(edit, &QPushButton::clicked, this, &MainWindow::editFilters); connect(preview, &QPushButton::clicked, this, &MainWindow::previewFilters);
    filterActions->addWidget(edit); filterActions->addWidget(preview); filterActions->addStretch();
    filterLayout->addWidget(filterSummary_); filterLayout->addWidget(previewSummary_); filterLayout->addLayout(filterActions); layout->addWidget(filter); updateFilterSummary();
    auto *run = new QHBoxLayout; run->addStretch(); backupButton_ = button(tr("开始备份"), true); backupButton_->setMinimumWidth(150);
    connect(backupButton_, &QPushButton::clicked, this, &MainWindow::startBackup); run->addWidget(backupButton_); layout->addLayout(run); layout->addStretch(); return page;
}

QWidget *MainWindow::createRestorePage() {
    QVBoxLayout *layout = nullptr; QWidget *page = createPageShell(tr("恢复数据"), tr("从 DBackup 归档恢复文件，并在需要时输入解密密码。"), &layout);
    auto *form = new QGroupBox(tr("恢复设置")); auto *grid = new QGridLayout(form);
    archivePath_ = new QLineEdit; restoreTarget_ = new QLineEdit; restorePassword_ = new QLineEdit; restorePassword_->setEchoMode(QLineEdit::Password);
    auto *archiveBrowse = button(tr("浏览…")); auto *targetBrowse = button(tr("浏览…"));
    connect(archiveBrowse, &QPushButton::clicked, this, &MainWindow::chooseArchive); connect(targetBrowse, &QPushButton::clicked, this, &MainWindow::chooseRestoreFolder);
    grid->addWidget(new QLabel(tr("归档文件")), 0, 0); grid->addWidget(archivePath_, 0, 1); grid->addWidget(archiveBrowse, 0, 2);
    grid->addWidget(new QLabel(tr("恢复到")), 1, 0); grid->addWidget(restoreTarget_, 1, 1); grid->addWidget(targetBrowse, 1, 2);
    grid->addWidget(new QLabel(tr("解密密码")), 2, 0); grid->addWidget(restorePassword_, 2, 1); layout->addWidget(form);
    auto *hint = new QLabel(tr("为避免误覆盖，请优先选择空目录。归档内路径会先经过安全校验，再写入目标目录。")); hint->setWordWrap(true); hint->setProperty("muted", true); layout->addWidget(hint);
    auto *run = new QHBoxLayout; run->addStretch(); restoreButton_ = button(tr("开始恢复"), true); restoreButton_->setMinimumWidth(150);
    connect(restoreButton_, &QPushButton::clicked, this, &MainWindow::startRestore); run->addWidget(restoreButton_); layout->addLayout(run); layout->addStretch(); return page;
}

QWidget *MainWindow::createSchedulePage() {
    QVBoxLayout *layout=nullptr;QWidget *page=createPageShell(tr("任务计划"),tr("持久化 Cron 计划、实时模式和版本保留策略。密码安全保存在 Windows 凭据管理器。"),&layout);
    auto *table=new QTableWidget(0,6);table->setHorizontalHeaderLabels({tr("名称"),tr("Cron"),tr("下次运行"),tr("保留"),tr("状态"),tr("最近结果")});table->setSelectionBehavior(QAbstractItemView::SelectRows);table->horizontalHeader()->setStretchLastSection(true);
    auto refresh=[this,table]{table->setRowCount(0);for(const auto&j:jobs_->jobs()){int row=table->rowCount();table->insertRow(row);QStringList values={j.name,j.cron,j.nextRun.toString("yyyy-MM-dd HH:mm"),QString::number(j.retention),j.enabled?tr("启用"):tr("停用"),j.lastResult};for(int c=0;c<values.size();++c){auto*item=new QTableWidgetItem(values[c]);item->setData(Qt::UserRole,j.id);table->setItem(row,c,item);}}};
    layout->addWidget(table);auto *actions=new QHBoxLayout;auto *add=button(tr("新建计划"),true),*remove=button(tr("删除")),*reload=button(tr("刷新"));actions->addWidget(add);actions->addWidget(remove);actions->addWidget(reload);actions->addStretch();layout->addLayout(actions);
    connect(reload,&QPushButton::clicked,this,refresh);connect(add,&QPushButton::clicked,this,[this,refresh]{BackupJob job;bool ok=false;job.name=QInputDialog::getText(this,tr("新建计划"),tr("任务名称"),QLineEdit::Normal,tr("自动备份"),&ok);if(!ok)return;QString source=QFileDialog::getExistingDirectory(this,tr("选择监控来源"));if(source.isEmpty())return;job.sources=QStringList{source};job.destination=QFileDialog::getExistingDirectory(this,tr("选择增量仓库目录"));if(job.destination.isEmpty())return;job.cron=QInputDialog::getText(this,tr("运行计划"),tr("5 段 Cron 表达式"),QLineEdit::Normal,"0 2 * * *",&ok);if(!ok)return;job.retention=QInputDialog::getInt(this,tr("保留策略"),tr("保留最近几个快照"),3,1,999,1,&ok);if(!ok)return;job.realtime=QMessageBox::question(this,tr("实时备份"),tr("是否同时监控目录变化并自动创建快照？"))==QMessageBox::Yes;QString password=QInputDialog::getText(this,tr("仓库密码"),tr("密码"),QLineEdit::Password,{},&ok);if(!ok||password.isEmpty())return;QString error;if(!jobs_->upsert(job,error)){QMessageBox::warning(this,tr("无法保存任务"),error);return;}const auto&saved=jobs_->jobs().last();std::string detail;CredentialStore::save(("job/"+saved.id).toStdString(),password.toUtf8().toStdString(),detail);refresh();});
    connect(remove,&QPushButton::clicked,this,[this,table,refresh]{int row=table->currentRow();if(row<0)return;QString id=table->item(row,0)->data(Qt::UserRole).toString(),error;jobs_->remove(id,error);std::string detail;CredentialStore::remove(("job/"+id).toStdString(),detail);refresh();});refresh();return page;
}

QWidget *MainWindow::createHistoryPage() {
    QVBoxLayout *layout=nullptr;QWidget *page=createPageShell(tr("备份历史"),tr("浏览本地内容寻址仓库中的版本，并按快照恢复或执行保留策略。"),&layout);
    auto *path=new QLineEdit,*password=new QLineEdit;password->setEchoMode(QLineEdit::Password);auto *browse=button(tr("选择仓库…")),*load=button(tr("加载历史"),true);auto *row=new QHBoxLayout;row->addWidget(path,1);row->addWidget(browse);row->addWidget(password);row->addWidget(load);layout->addLayout(row);
    auto *table=new QTableWidget(0,3);table->setHorizontalHeaderLabels({tr("快照 ID"),tr("创建时间"),tr("状态")});table->horizontalHeader()->setStretchLastSection(true);layout->addWidget(table);
    connect(browse,&QPushButton::clicked,this,[this,path]{QString p=QFileDialog::getExistingDirectory(this,tr("选择仓库"));if(!p.isEmpty())path->setText(p);});connect(load,&QPushButton::clicked,this,[this,path,table]{LocalRepository repo(path->text().toUtf8().toStdString());std::vector<SnapshotInfo>all;std::string error;if(!repo.listSnapshots(all,error)){QMessageBox::warning(this,tr("读取失败"),QString::fromUtf8(error.c_str()));return;}table->setRowCount(0);for(auto&s:all){int r=table->rowCount();table->insertRow(r);table->setItem(r,0,new QTableWidgetItem(QString::fromStdString(s.id)));table->setItem(r,1,new QTableWidgetItem(QDateTime::fromMSecsSinceEpoch(s.createdAtMs).toString("yyyy-MM-dd HH:mm:ss")));table->setItem(r,2,new QTableWidgetItem(tr("可恢复")));}});
    auto *restore=button(tr("恢复所选快照"),true);layout->addWidget(restore,0,Qt::AlignRight);connect(restore,&QPushButton::clicked,this,[this,path,password,table]{if(table->currentRow()<0)return;QString dest=QFileDialog::getExistingDirectory(this,tr("恢复到"));if(dest.isEmpty())return;LocalRepository repo(path->text().toUtf8().toStdString());std::string error;bool ok=repo.restoreSnapshot(table->item(table->currentRow(),0)->text().toStdString(),dest.toUtf8().toStdString(),password->text().toUtf8().toStdString(),error);QMessageBox::information(this,ok?tr("恢复完成"):tr("恢复失败"),ok?tr("快照已恢复"):QString::fromUtf8(error.c_str()));});return page;
}

QWidget *MainWindow::createStoragePage() {
    QVBoxLayout *layout=nullptr;QWidget *page=createPageShell(tr("存储位置"),tr("管理本地增量仓库和远程 DBackup Server。"),&layout);
    auto *local=new QGroupBox(tr("本地仓库"));auto *localLayout=new QVBoxLayout(local);auto *localText=new QLabel(tr("本地仓库使用 4 MiB 内容寻址块、SHA-256 去重及 AES-256-GCM 认证加密。可在“新建备份”或“任务计划”中创建。"));localText->setWordWrap(true);localLayout->addWidget(localText);layout->addWidget(local);
    auto *remote=new QGroupBox(tr("远程服务器"));auto *form=new QFormLayout(remote);
    auto *endpoint=new QLineEdit("https://127.0.0.1:8443");auto *user=new QLineEdit;auto *pass=new QLineEdit;auto *status=new QLabel(tr("未连接"));
    pass->setEchoMode(QLineEdit::Password);form->addRow(tr("服务器"),endpoint);form->addRow(tr("用户名"),user);form->addRow(tr("密码"),pass);
    auto *buttons=new QHBoxLayout;auto *registerButton=button(tr("注册"));auto *loginButton=button(tr("登录"),true);auto *logoutButton=button(tr("退出登录"));
    buttons->addWidget(registerButton);buttons->addWidget(loginButton);buttons->addWidget(logoutButton);buttons->addWidget(status,1);form->addRow(buttons);
    auto *snapshots=new QListWidget;form->addRow(tr("远程快照"),snapshots);auto *remoteActions=new QHBoxLayout;auto *refreshButton=button(tr("刷新"));auto *uploadButton=button(tr("上传本地快照"),true);auto *downloadButton=button(tr("下载到本地仓库"));auto *deleteButton=button(tr("删除"));remoteActions->addWidget(refreshButton);remoteActions->addWidget(uploadButton);remoteActions->addWidget(downloadButton);remoteActions->addWidget(deleteButton);form->addRow(remoteActions);
    auto *note=new QLabel(tr("连接使用 TLS 加密，备份数据按账户隔离存储。"));note->setWordWrap(true);note->setProperty("muted",true);form->addRow(note);layout->addWidget(remote);layout->addStretch();
    auto configure=[this,endpoint]{remote_->configure(QUrl(endpoint->text()),"remote/"+QUrl(endpoint->text()).host());};
    auto refresh=[this,snapshots,status,configure]{configure();remote_->listSnapshots([snapshots,status](QJsonArray values,QString error){snapshots->clear();if(!error.isEmpty()){status->setText(error);return;}for(auto value:values){auto object=value.toObject();auto *item=new QListWidgetItem(object["name"].toString(object["id"].toString()));item->setData(Qt::UserRole,object["id"].toString());snapshots->addItem(item);}status->setText(QObject::tr("已连接，%1 个快照").arg(values.size()));});};
    connect(registerButton,&QPushButton::clicked,this,[=]{configure();remote_->registerUser(user->text(),pass->text(),[status](bool ok,QString error){status->setText(ok?QObject::tr("注册成功，请登录"):error);});});connect(loginButton,&QPushButton::clicked,this,[=]{configure();remote_->login(user->text(),pass->text(),[=](bool ok,QString error){status->setText(ok?QObject::tr("已连接"):error);if(ok)refresh();});});connect(logoutButton,&QPushButton::clicked,this,[=]{remote_->logout([status,snapshots](bool ok,QString error){status->setText(ok?QObject::tr("已退出"):error);if(ok)snapshots->clear();});});connect(refreshButton,&QPushButton::clicked,this,refresh);
    connect(uploadButton,&QPushButton::clicked,this,[=]{QString root=QFileDialog::getExistingDirectory(this,tr("选择本地增量仓库"));if(root.isEmpty())return;QDir directory(root+"/snapshots");QStringList manifests=directory.entryList({"*.manifest"},QDir::Files,QDir::Time);if(manifests.isEmpty()){QMessageBox::information(this,tr("无可用快照"),tr("所选仓库中没有快照。"));return;}QStringList ids;for(auto name:manifests)ids<<name.left(name.size()-9);bool ok=false;QString id=QInputDialog::getItem(this,tr("选择快照"),tr("快照"),ids,0,false,&ok);if(!ok)return;cancelled_=false;setBusy(true,tr("正在上传快照…"));remoteRepository_->uploadSnapshot(root,id,&cancelled_,[this](int value,QString path){progress_->setValue(value);progressDetails_->setText(tr("正在上传 · %1").arg(path));},[=](bool success,QString error){setBusy(false);if(success){QMessageBox::information(this,tr("上传完成"),tr("快照已保存到远程服务器。"));refresh();}else QMessageBox::warning(this,tr("上传失败"),error);});});
    connect(downloadButton,&QPushButton::clicked,this,[=]{auto *item=snapshots->currentItem();if(!item)return;QString root=QFileDialog::getExistingDirectory(this,tr("选择本地仓库目录"));if(root.isEmpty())return;cancelled_=false;setBusy(true,tr("正在下载快照…"));remoteRepository_->downloadSnapshot(item->data(Qt::UserRole).toString(),root,&cancelled_,[this](int value,QString path){progress_->setValue(value);progressDetails_->setText(tr("正在下载 · %1").arg(path));},[this](bool success,QString error){setBusy(false);if(success)QMessageBox::information(this,tr("下载完成"),tr("快照已保存到本地仓库。"));else QMessageBox::warning(this,tr("下载失败"),error);});});
    connect(deleteButton,&QPushButton::clicked,this,[=]{auto *item=snapshots->currentItem();if(!item)return;if(QMessageBox::question(this,tr("删除快照"),tr("确定删除选中的远程快照？"))!=QMessageBox::Yes)return;remote_->deleteSnapshot(item->data(Qt::UserRole).toString(),[=](bool ok,QString error){if(ok)refresh();else QMessageBox::warning(this,tr("删除失败"),error);});});return page;
}

QWidget *MainWindow::createSettingsPage(){QVBoxLayout *layout=nullptr;QWidget *page=createPageShell(tr("设置"),tr("设置应用启动和后台运行方式。"),&layout);auto *group=new QGroupBox(tr("系统集成"));auto *box=new QVBoxLayout(group);auto *startup=new QCheckBox(tr("登录 Windows 后自动启动 DBackup"));QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",QSettings::NativeFormat);startup->setChecked(run.contains("DBackup"));connect(startup,&QCheckBox::toggled,this,[](bool enabled){QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",QSettings::NativeFormat);if(enabled)settings.setValue("DBackup",QString("\"")+QCoreApplication::applicationFilePath()+"\"");else settings.remove("DBackup");});box->addWidget(startup);auto *tray=new QLabel(tr("关闭主窗口时程序保持在系统托盘；请使用托盘菜单中的“退出”完全停止。"));tray->setWordWrap(true);tray->setProperty("muted",true);box->addWidget(tray);layout->addWidget(group);layout->addStretch();return page;}

void MainWindow::addSources(bool directory) {
    QStringList paths;
    if (directory) { const QString path = QFileDialog::getExistingDirectory(this, tr("选择备份文件夹")); if (!path.isEmpty()) paths << path; }
    else paths = QFileDialog::getOpenFileNames(this, tr("选择一个或多个备份文件"));
    for (const QString &path : paths) {
        bool exists = false; for (int i = 0; i < sourceList_->count(); ++i) if (sourceList_->item(i)->data(Qt::UserRole).toString() == path) exists = true;
        if (!exists) { auto *item = new QListWidgetItem((QFileInfo(path).isDir() ? tr("文件夹  ") : tr("文件  ")) + path); item->setData(Qt::UserRole, path); sourceList_->addItem(item); }
    }
    updateSourceSummary();
}

void MainWindow::removeSelectedSources() { qDeleteAll(sourceList_->selectedItems()); updateSourceSummary(); }
void MainWindow::updateSourceSummary() { sourceSummary_->setText(tr("%1 个来源").arg(sourceList_->count())); previewSummary_->setText(tr("来源或规则已更改，请重新扫描预览")); }
void MainWindow::chooseBackupTarget() {if(backupMode_->currentIndex()==1){QString path=QFileDialog::getExistingDirectory(this,tr("选择或创建增量仓库目录"));if(!path.isEmpty())backupTarget_->setText(path);return;} QString path = QFileDialog::getSaveFileName(this, tr("保存备份归档"), {}, tr("DBackup 归档 (*.abk)")); if (!path.isEmpty()) { if (!path.endsWith(".abk", Qt::CaseInsensitive)) path += ".abk"; backupTarget_->setText(path); } }
void MainWindow::chooseArchive() { const QString path = QFileDialog::getOpenFileName(this, tr("选择备份归档"), {}, tr("DBackup 归档 (*.abk);;所有文件 (*)")); if (!path.isEmpty()) archivePath_->setText(path); }
void MainWindow::chooseRestoreFolder() { const QString path = QFileDialog::getExistingDirectory(this, tr("选择恢复目录")); if (!path.isEmpty()) restoreTarget_->setText(path); }

void MainWindow::editFilters() { FilterDialog dialog(filters_, this); if (dialog.exec() == QDialog::Accepted) { filters_ = dialog.options(); updateFilterSummary(); previewSummary_->setText(tr("规则已更改，请重新扫描预览")); } }
void MainWindow::updateFilterSummary() {
    QStringList parts;
    if (!filters_.extensions.empty()) parts << tr("%1 种扩展名").arg(filters_.extensions.size());
    if (!filters_.pathContains.empty()) parts << tr("路径包含“%1”").arg(QString::fromUtf8(filters_.pathContains.c_str()));
    if (!filters_.nameContains.empty()) parts << tr("名称包含“%1”").arg(QString::fromUtf8(filters_.nameContains.c_str()));
    if (filters_.minSize) parts << tr("≥ %1").arg(humanSize(filters_.minSize)); if (filters_.maxSize) parts << tr("≤ %1").arg(humanSize(filters_.maxSize));
    if (!filters_.pathRules.empty()) parts << tr("%1 条包含/排除规则").arg(filters_.pathRules.size());
    filterSummary_->setText(parts.isEmpty() ? tr("未设置筛选：将包含所有可读取的项目") : parts.join(tr(" · ")));
}

std::vector<std::string> MainWindow::sourcePaths() const { std::vector<std::string> paths; for (int i = 0; i < sourceList_->count(); ++i) paths.push_back(sourceList_->item(i)->data(Qt::UserRole).toString().toUtf8().toStdString()); return paths; }

void MainWindow::previewFilters() {
    if (busy_) return;
    const auto sources = sourcePaths(); if (sources.empty()) { QMessageBox::information(this, tr("扫描预览"), tr("请先添加至少一个备份来源。")); return; }
    setBusy(true, tr("正在扫描文件元数据…")); const FilterOptions filters = filters_;
    QThread *worker = QThread::create([this, sources, filters] {
        BackupPreview preview; std::string error; const bool ok = Packer::preview(sources, filters, preview, error);
        QMetaObject::invokeMethod(this, [this, ok, preview, error] {
            setBusy(false); if (!ok) { QMessageBox::warning(this, tr("预览失败"), QString::fromUtf8(error.c_str())); appendLog(tr("扫描预览失败：%1").arg(QString::fromUtf8(error.c_str())), true); return; }
            previewSummary_->setText(tr("预计包含 %1 个文件、%2 个文件夹，共 %3；排除 %4 个文件")
                .arg(preview.includedFiles).arg(preview.includedDirectories).arg(humanSize(preview.includedBytes)).arg(preview.excludedFiles));
            appendLog(tr("扫描预览完成：%1 个文件，%2").arg(preview.includedFiles).arg(humanSize(preview.includedBytes)));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
}

void MainWindow::startBackup() {
    if (busy_) return;
    const auto sources = sourcePaths(); const QString target = backupTarget_->text().trimmed();
    if (sources.empty() || target.isEmpty()) { QMessageBox::information(this, tr("无法开始备份"), tr("请添加备份来源并选择归档保存位置。")); return; }
    PackOptions options; options.compressionLevel = compression_->value(); options.password = backupPassword_->text().toUtf8().toStdString(); options.filter = filters_;bool incremental=backupMode_->currentIndex()==1;
    if(incremental&&options.password.empty()){QMessageBox::information(this,tr("需要仓库密码"),tr("增量仓库必须设置密码，以保护数据块和快照清单。"));return;}
    cancelled_=false;setBusy(true, tr("正在创建备份归档…")); appendLog(tr("开始备份到 %1").arg(target));
    QThread *worker = QThread::create([this, sources, target, options,incremental] {
        OperationContext context;context.cancelled=&cancelled_;context.onProgress=[this](const OperationProgress&p){QMetaObject::invokeMethod(this,[this,p]{updateProgress(p);},Qt::QueuedConnection);};
        std::string error;bool ok=false;if(incremental){LocalRepository repository(target.toUtf8().toStdString());SnapshotOptions snapshot;snapshot.sources=sources;snapshot.password=options.password;snapshot.filter=options.filter;SnapshotInfo info;ok=repository.createSnapshot(snapshot,info,error,context);}else ok=Packer::pack(sources, target.toUtf8().toStdString(), options, error,context);
        QMetaObject::invokeMethod(this, [this, ok, target, error] { setBusy(false); if (ok) { appendLog(tr("备份完成：%1").arg(target)); QMessageBox::information(this, tr("备份完成"), tr("归档已安全写入：\n%1").arg(target)); }
            else { appendLog(tr("备份失败：%1").arg(QString::fromUtf8(error.c_str())), true); QMessageBox::critical(this, tr("备份失败"), QString::fromUtf8(error.c_str())); } }, Qt::QueuedConnection);
    }); connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
}

void MainWindow::startRestore() {
    if (busy_) return;
    const QString archive = archivePath_->text().trimmed(), target = restoreTarget_->text().trimmed(), password = restorePassword_->text();
    if (archive.isEmpty() || target.isEmpty()) { QMessageBox::information(this, tr("无法开始恢复"), tr("请选择归档文件和恢复目录。")); return; }
    cancelled_=false;setBusy(true, tr("正在恢复文件…")); appendLog(tr("开始从 %1 恢复").arg(archive));
    QThread *worker = QThread::create([this, archive, target, password] {OperationContext context;context.cancelled=&cancelled_;context.onProgress=[this](const OperationProgress&p){QMetaObject::invokeMethod(this,[this,p]{updateProgress(p);},Qt::QueuedConnection);}; std::string error; const bool ok = Packer::unpack(archive.toUtf8().toStdString(), target.toUtf8().toStdString(), error, password.toUtf8().toStdString(),context);
        QMetaObject::invokeMethod(this, [this, ok, target, error] { setBusy(false); if (ok) { appendLog(tr("恢复完成：%1").arg(target)); QMessageBox::information(this, tr("恢复完成"), tr("文件已恢复到：\n%1").arg(target)); }
            else { appendLog(tr("恢复失败：%1").arg(QString::fromUtf8(error.c_str())), true); QMessageBox::critical(this, tr("恢复失败"), QString::fromUtf8(error.c_str())); } }, Qt::QueuedConnection);
    }); connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
}

void MainWindow::updateProgress(const OperationProgress &p) {
    int percent = 0;
    if (p.totalBytes) percent = static_cast<int>(100 * p.completedBytes / p.totalBytes);
    else if (p.totalFiles) percent = static_cast<int>(100 * p.completedFiles / p.totalFiles);
    progress_->setValue(qBound(0, percent, 100));
    QStringList details{stageName(p.stage)};
    if (p.totalFiles) details << tr("%1/%2 个项目").arg(p.completedFiles).arg(p.totalFiles);
    if (p.totalBytes) details << tr("%1/%2").arg(humanSize(p.completedBytes), humanSize(p.totalBytes));
    if (p.bytesPerSecond > 0) {
        details << tr("%1/s").arg(humanSize(static_cast<uint64_t>(p.bytesPerSecond)));
        if (p.totalBytes > p.completedBytes) details << tr("剩余约 %1 秒").arg(static_cast<qulonglong>((p.totalBytes-p.completedBytes)/p.bytesPerSecond));
    }
    progressDetails_->setText(details.join(QStringLiteral(" · ")));
    if (!p.currentPath.empty()) statusBar()->showMessage(QString::fromUtf8(p.currentPath.c_str()));
}

void MainWindow::setBusy(bool busy, const QString &message) { busy_ = busy; backupButton_->setEnabled(!busy); restoreButton_->setEnabled(!busy);cancelButton_->setVisible(busy); if (busy) { progress_->setRange(0, 100);progress_->setValue(0);progressDetails_->setText(message); statusBar()->showMessage(message); } else { progress_->setRange(0, 100); progress_->setValue(0);progressDetails_->setText(tr("就绪")); statusBar()->showMessage(tr("就绪")); } }
void MainWindow::appendLog(const QString &message, bool error) { activityLog_->appendPlainText(QString("[%1] %2%3").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), error ? tr("错误：") : QString(), message)); }
