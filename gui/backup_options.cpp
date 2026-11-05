#include "gui/backup_options.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QAbstractButton>

namespace {
QWidget *choices(QButtonGroup *group, std::initializer_list<std::pair<QString, int>> items,
                 int selected, const char *prefix) {
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    for (const auto &item : items) {
        auto *radio = new QRadioButton(item.first);
        radio->setObjectName(QString::fromLatin1(prefix) + QString::number(item.second));
        group->addButton(radio, item.second);
        radio->setChecked(item.second == selected);
        layout->addWidget(radio);
    }
    layout->addStretch();
    return container;
}
}

BackupOptionsWidget::BackupOptionsWidget(QWidget *parent, bool allowRepository) : QWidget(parent) {
    allowRepository_=allowRepository;
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    mode_ = new QButtonGroup(this);
    format_ = new QButtonGroup(this);
    compression_ = new QButtonGroup(this);
    auto *modeLabel=new QLabel(tr("备份模式"));
    auto *modeChoices=choices(mode_, {{tr("完整归档"), 0}, {tr("增量仓库"), 1}}, 0, "backupMode");
    modeLabel->setVisible(allowRepository);modeChoices->setVisible(allowRepository);
    grid->addWidget(modeLabel, 0, 0);
    grid->addWidget(modeChoices, 0, 1, 1, 3);
    grid->addWidget(new QLabel(tr("打包")), 1, 0);
    formatChoices_ = choices(format_, {{tr("自定义"), 0}, {tr("tar"), 1}}, 0, "packFormat");
    grid->addWidget(formatChoices_, 1, 1, 1, 3);
    compress_ = new QCheckBox(tr("压缩"));
    compress_->setObjectName("compressionEnabled");
    compress_->setChecked(true);
    grid->addWidget(compress_, 2, 0);
    compressionChoices_ = choices(compression_, {{tr("Huffman"), 1}, {tr("LZ77"), 2}, {tr("DEFLATE"), 3}}, 3, "compression");
    grid->addWidget(compressionChoices_, 2, 1, 1, 3);
    encrypt_ = new QCheckBox(tr("加密"));
    encrypt_->setObjectName("encryptionEnabled");
    grid->addWidget(encrypt_, 3, 0);
    repositoryEncryption_ = new QLabel(tr("加密"));
    grid->addWidget(repositoryEncryption_, 3, 0);
    auto *algorithm = new QLabel(tr("AES-256-GCM"));
    grid->addWidget(algorithm, 3, 1);
    password_ = new QLineEdit;
    password_->setObjectName("backupPassword");
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("密码"));
    grid->addWidget(password_, 3, 2);
    showPassword_ = new QCheckBox(tr("显示"));
    grid->addWidget(showPassword_, 3, 3);
    repositoryHint_ = new QLabel(tr("增量仓库使用分块与 AES 加密"));
    repositoryHint_->setProperty("muted", true);
    grid->addWidget(repositoryHint_, 4, 1, 1, 3);
    outputHint_ = new QLabel; outputHint_->setProperty("muted", true); outputHint_->setWordWrap(true);
    grid->addWidget(outputHint_, 5, 1, 1, 3);
    connect(mode_, &QButtonGroup::idToggled, this, [this](int, bool) { updateEnabled(); });
    connect(format_, &QButtonGroup::idToggled, this, [this](int, bool) { updateEnabled(); });
    connect(compress_, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(encrypt_, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(showPassword_, &QCheckBox::toggled, this, [this](bool show) {
        password_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    updateEnabled();
}

bool BackupOptionsWidget::incremental() const { return mode_->checkedId() == 1; }
bool BackupOptionsWidget::encryptionEnabled() const { return incremental() || encrypt_->isChecked(); }

void BackupOptionsWidget::setOptions(bool repository, bool encrypted, PackFormat format, Compression compression) {
    if (auto *b=mode_->button(repository && allowRepository_ ? 1 : 0)) b->setChecked(true);
    if (auto *b=format_->button(static_cast<int>(format))) b->setChecked(true);
    compress_->setChecked(compression != Compression::None);
    if (auto *b=compression_->button(static_cast<int>(compression))) b->setChecked(true);
    encrypt_->setChecked(encrypted);
    updateEnabled();
}

PackOptions BackupOptionsWidget::options() const {
    PackOptions result;
    result.format = static_cast<PackFormat>(format_->checkedId());
    result.compression = compress_->isChecked() ? static_cast<Compression>(compression_->checkedId()) : Compression::None;
    if (encryptionEnabled()) result.password = password_->text().toUtf8().toStdString();
    return result;
}

void BackupOptionsWidget::updateEnabled() {
    const bool full = !incremental();
    formatChoices_->setEnabled(full);
    compress_->setEnabled(full);
    compressionChoices_->setEnabled(full && compress_->isChecked());
    encrypt_->setEnabled(full);
    encrypt_->setVisible(full);
    repositoryEncryption_->setVisible(!full);
    password_->setEnabled(encryptionEnabled());
    showPassword_->setEnabled(encryptionEnabled());
    repositoryHint_->setVisible(!full);
    if (!full) outputHint_->setText(tr("输出：版本仓库，使用 DBackup 恢复"));
    else if (format_->checkedId() == static_cast<int>(PackFormat::Tar) && !compress_->isChecked() && !encrypt_->isChecked())
        outputHint_->setText(tr("输出：标准 tar，可使用系统工具打开"));
    else if (format_->checkedId() == static_cast<int>(PackFormat::Tar)) outputHint_->setText(tr("tar 打包 + 压缩或加密：输出 .abk，使用 DBackup 恢复"));
    else outputHint_->setText(tr("输出：DBackup 归档（.abk），使用 DBackup 恢复"));
}
