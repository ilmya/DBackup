// 备份方式、格式、压缩和加密选项界面
// 构造完整与增量备份选项，联动格式、压缩、加密、密码及实时计划的模式限制。
#include "gui/backup_options.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>

namespace {
// 按给定项目生成一组互斥单选项，并返回其布局容器
QWidget* choices(QButtonGroup* group, std::initializer_list<std::pair<QString, int>> items,
                 int selected, const char* prefix) {
    auto* container = new QWidget;
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    for (const auto& item : items) {
        auto* radio = new QRadioButton(item.first);
        radio->setObjectName(QString::fromLatin1(prefix) + QString::number(item.second));
        group->addButton(radio, item.second);
        radio->setChecked(item.second == selected);
        layout->addWidget(radio);
    }
    layout->addStretch();
    return container;
}
} // namespace

// 创建各组选项并连接状态变化，普通备份页面可隐藏模式选择。
BackupOptionsWidget::BackupOptionsWidget(QWidget* parent, bool allowIncremental) : QWidget(parent) {
    allowIncremental_ = allowIncremental;
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    mode_ = new QButtonGroup(this);
    format_ = new QButtonGroup(this);
    compression_ = new QButtonGroup(this);
    auto* modeLabel = new QLabel(tr("备份模式"));
    auto* modeChoices = choices(mode_, {{tr("完整备份"), 0}, {tr("增量备份"), 1}}, 0,
                                "backupMode");
    modeLabel->setVisible(allowIncremental);
    modeChoices->setVisible(allowIncremental);
    grid->addWidget(modeLabel, 0, 0);
    grid->addWidget(modeChoices, 0, 1, 1, 3);
    grid->addWidget(new QLabel(tr("打包")), 1, 0);
    formatChoices_ = choices(format_, {{tr("自定义"), 0}, {tr("tar"), 1}}, 0, "packFormat");
    grid->addWidget(formatChoices_, 1, 1, 1, 3);
    compress_ = new QCheckBox(tr("压缩"));
    compress_->setObjectName("compressionEnabled");
    compress_->setChecked(true);
    grid->addWidget(compress_, 2, 0);
    compressionChoices_ = choices(
        compression_, {{tr("Huffman"), 1}, {tr("LZ77"), 2}, {tr("DEFLATE"), 3}}, 3, "compression");
    grid->addWidget(compressionChoices_, 2, 1, 1, 3);
    encrypt_ = new QCheckBox(tr("加密"));
    encrypt_->setObjectName("encryptionEnabled");
    grid->addWidget(encrypt_, 3, 0);
    auto* algorithm = new QLabel(tr("AES-256-GCM"));
    grid->addWidget(algorithm, 3, 1);
    password_ = new QLineEdit;
    password_->setObjectName("backupPassword");
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("密码"));
    grid->addWidget(password_, 3, 2);
    showPassword_ = new QCheckBox(tr("显示"));
    grid->addWidget(showPassword_, 3, 3);
    connect(mode_, &QButtonGroup::idToggled, this, [this](int, bool) { updateEnabled(); });
    connect(format_, &QButtonGroup::idToggled, this, [this](int, bool) { updateEnabled(); });
    connect(compress_, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(encrypt_, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(showPassword_, &QCheckBox::toggled, this, [this](bool show) {
        password_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    updateEnabled();
}

// 返回计划当前是否选择增量备份。
bool BackupOptionsWidget::incremental() const {
    return mode_->checkedId() == 1;
}
// 返回用户是否启用密码加密。
bool BackupOptionsWidget::encryptionEnabled() const {
    return encrypt_->isChecked();
}

// 实时计划启用时强制增量模式，解除限制后恢复原先选择。
void BackupOptionsWidget::setIncrementalRequired(bool required) {
    if (required == incrementalRequired_ || !allowIncremental_)
        return;
    if (required) {
        previousMode_ = mode_->checkedId();
        if (previousMode_ == 0)
            mode_->button(1)->setChecked(true);
    }
    mode_->button(0)->setEnabled(!required);
    incrementalRequired_ = required;
    if (!required)
        mode_->button(previousMode_)->setChecked(true);
}

// 根据已保存计划恢复备份模式、格式、压缩及加密设置。
void BackupOptionsWidget::setOptions(bool incremental, bool encrypted, PackFormat format,
                                     Compression compression) {
    if (auto* b = mode_->button(allowIncremental_ && incremental ? 1 : 0))
        b->setChecked(true);
    if (auto* b = format_->button(static_cast<int>(format)))
        b->setChecked(true);
    compress_->setChecked(compression != Compression::None);
    if (auto* b = compression_->button(static_cast<int>(compression)))
        b->setChecked(true);
    encrypt_->setChecked(encrypted);
    updateEnabled();
}

// 汇总当前打包选项，仅在启用加密时读取密码。
PackOptions BackupOptionsWidget::options() const {
    PackOptions result;
    result.format = static_cast<PackFormat>(format_->checkedId());
    result.compression = compress_->isChecked()
                             ? static_cast<Compression>(compression_->checkedId())
                             : Compression::None;
    if (encryptionEnabled())
        result.password = password_->text().toUtf8().toStdString();
    return result;
}

// 将密钥环中读取的密码填入密码输入框。
void BackupOptionsWidget::setPassword(const QString& password) {
    password_->setText(password);
}

// 根据压缩和加密开关更新算法选择与密码输入状态。
void BackupOptionsWidget::updateEnabled() {
    formatChoices_->setEnabled(true);
    compress_->setEnabled(true);
    compressionChoices_->setEnabled(compress_->isChecked());
    encrypt_->setEnabled(true);
    encrypt_->setVisible(true);
    password_->setEnabled(encryptionEnabled());
    showPassword_->setEnabled(encryptionEnabled());
}
