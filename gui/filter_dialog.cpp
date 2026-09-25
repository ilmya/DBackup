#include "gui/filter_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QTimeZone>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
uint64_t unitMultiplier(int index) {
    static constexpr uint64_t values[] = {1ULL, 1024ULL, 1024ULL * 1024ULL, 1024ULL * 1024ULL * 1024ULL};
    return values[index < 0 || index > 3 ? 0 : index];
}

void setSizeValue(uint64_t bytes, QSpinBox *value, QComboBox *unit) {
    int selected = 0;
    while (selected < 3 && bytes > 0 && bytes % 1024 == 0) {
        bytes /= 1024;
        ++selected;
    }
    value->setValue(static_cast<int>(qMin<uint64_t>(bytes, 2147483647ULL)));
    unit->setCurrentIndex(selected);
}

QStringList ruleLines(const QPlainTextEdit *edit) {
    QStringList result;
    for (const QString &line : edit->toPlainText().split('\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) result << trimmed;
    }
    return result;
}
}

FilterDialog::FilterDialog(const FilterOptions &options, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("筛选规则"));
    setMinimumSize(720, 590);
    auto *root = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("仅备份符合全部基础条件的项目；排除规则始终优先。通配符支持 * 和 ?。"));
    intro->setProperty("muted", true);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *tabs = new QTabWidget;
    auto *basic = new QWidget;
    auto *form = new QFormLayout(basic);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    pathEdit_ = new QLineEdit(QString::fromUtf8(options.pathContains.c_str()));
    pathEdit_->setPlaceholderText(tr("例如：documents 或 projects/2026"));
    nameEdit_ = new QLineEdit(QString::fromUtf8(options.nameContains.c_str()));
    nameEdit_->setPlaceholderText(tr("文件名包含的文字"));
    extensionsEdit_ = new QLineEdit;
    QStringList initialExtensions;
    for (const auto &extension : options.extensions) initialExtensions << QString::fromUtf8(extension.c_str());
    if (initialExtensions.isEmpty() && !options.extension.empty()) initialExtensions << QString::fromUtf8(options.extension.c_str());
    extensionsEdit_->setText(initialExtensions.join(", "));
    extensionsEdit_->setPlaceholderText(tr("例如：.docx, .pdf, .md"));
    ownerEdit_ = new QLineEdit(QString::fromUtf8(options.ownerContains.c_str()));

    entryType_ = new QComboBox;
    entryType_->addItems({tr("文件和文件夹"), tr("仅文件"), tr("仅文件夹")});
    entryType_->setCurrentIndex(static_cast<int>(options.type));
    auto *preset = new QComboBox;
    preset->addItems({tr("自定义 / 不限制"), tr("文档"), tr("图片"), tr("音频"), tr("视频"), tr("压缩包"), tr("程序代码")});
    connect(preset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) { applyTypePreset(index); });

    auto *typeRow = new QHBoxLayout;
    typeRow->addWidget(entryType_, 1);
    typeRow->addWidget(new QLabel(tr("类型预设")));
    typeRow->addWidget(preset, 1);
    form->addRow(tr("路径包含"), pathEdit_);
    form->addRow(tr("名称包含"), nameEdit_);
    form->addRow(tr("扩展名"), extensionsEdit_);
    form->addRow(tr("项目类型"), typeRow);

    minSize_ = new QSpinBox;
    maxSize_ = new QSpinBox;
    for (auto *box : {minSize_, maxSize_}) { box->setRange(0, 2147483647); box->setSpecialValueText(tr("不限")); }
    minUnit_ = new QComboBox;
    maxUnit_ = new QComboBox;
    minUnit_->addItems({"B", "KB", "MB", "GB"});
    maxUnit_->addItems({"B", "KB", "MB", "GB"});
    setSizeValue(options.minSize, minSize_, minUnit_);
    setSizeValue(options.maxSize, maxSize_, maxUnit_);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(tr("最小")));
    sizeRow->addWidget(minSize_);
    sizeRow->addWidget(minUnit_);
    sizeRow->addSpacing(18);
    sizeRow->addWidget(new QLabel(tr("最大")));
    sizeRow->addWidget(maxSize_);
    sizeRow->addWidget(maxUnit_);
    form->addRow(tr("文件大小"), sizeRow);

    afterEnabled_ = new QCheckBox(tr("晚于"));
    beforeEnabled_ = new QCheckBox(tr("早于"));
    afterDate_ = new QDateEdit(QDate::currentDate().addDays(-7));
    beforeDate_ = new QDateEdit(QDate::currentDate());
    afterDate_->setCalendarPopup(true);
    beforeDate_->setCalendarPopup(true);
    afterEnabled_->setChecked(options.afterMtimeMs != 0);
    beforeEnabled_->setChecked(options.beforeMtimeMs != 0);
    if (options.afterMtimeMs) afterDate_->setDate(QDateTime::fromMSecsSinceEpoch(options.afterMtimeMs, QTimeZone::UTC).date());
    if (options.beforeMtimeMs) beforeDate_->setDate(QDateTime::fromMSecsSinceEpoch(options.beforeMtimeMs, QTimeZone::UTC).date());
    afterDate_->setEnabled(afterEnabled_->isChecked());
    beforeDate_->setEnabled(beforeEnabled_->isChecked());
    connect(afterEnabled_, &QCheckBox::toggled, afterDate_, &QWidget::setEnabled);
    connect(beforeEnabled_, &QCheckBox::toggled, beforeDate_, &QWidget::setEnabled);
    auto *dateRow = new QHBoxLayout;
    dateRow->addWidget(afterEnabled_); dateRow->addWidget(afterDate_);
    dateRow->addSpacing(18); dateRow->addWidget(beforeEnabled_); dateRow->addWidget(beforeDate_);
    form->addRow(tr("修改日期"), dateRow);
    auto *datePresets = new QHBoxLayout;
    for (const auto &presetValue : {qMakePair(tr("今天"), 0), qMakePair(tr("最近 7 天"), 6), qMakePair(tr("最近 30 天"), 29)}) {
        auto *quick = new QPushButton(presetValue.first);
        connect(quick, &QPushButton::clicked, this, [this, days = presetValue.second] {
            afterEnabled_->setChecked(true); beforeEnabled_->setChecked(true);
            afterDate_->setDate(QDate::currentDate().addDays(-days)); beforeDate_->setDate(QDate::currentDate());
        });
        datePresets->addWidget(quick);
    }
    datePresets->addStretch();
    form->addRow(tr("快捷时间"), datePresets);
    form->addRow(tr("所有者包含"), ownerEdit_);
    tabs->addTab(basic, tr("基础条件"));

    auto *rules = new QWidget;
    auto *rulesLayout = new QVBoxLayout(rules);
    auto *common = new QGroupBox(tr("常用排除"));
    auto *commonLayout = new QHBoxLayout(common);
    temporary_ = new QCheckBox(tr("临时文件"));
    cache_ = new QCheckBox(tr("系统缓存"));
    build_ = new QCheckBox(tr("编译输出"));
    logs_ = new QCheckBox(tr("日志文件"));
    for (auto *box : {temporary_, cache_, build_, logs_}) commonLayout->addWidget(box);
    commonLayout->addStretch();
    rulesLayout->addWidget(common);
    auto *editors = new QHBoxLayout;
    auto makeEditor = [&](const QString &title, QPlainTextEdit **target, const QString &hint) {
        auto *group = new QGroupBox(title);
        auto *layout = new QVBoxLayout(group);
        *target = new QPlainTextEdit;
        (*target)->setPlaceholderText(hint);
        layout->addWidget(*target);
        editors->addWidget(group);
    };
    makeEditor(tr("包含规则（每行一个）"), &includeRules_, tr("projects/*\nphotos/2026/*"));
    makeEditor(tr("排除规则（每行一个）"), &excludeRules_, tr("*.bak\nprivate/*\n*/cache/*"));
    for (const auto &rule : options.pathRules) {
        QPlainTextEdit *edit = rule.include ? includeRules_ : excludeRules_;
        edit->appendPlainText(QString::fromUtf8(rule.pattern.c_str()));
    }
    rulesLayout->addLayout(editors, 1);
    tabs->addTab(rules, tr("包含与排除"));
    root->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("应用规则"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("清空全部"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        const uint64_t minimum = static_cast<uint64_t>(minSize_->value()) * unitMultiplier(minUnit_->currentIndex());
        const uint64_t maximum = static_cast<uint64_t>(maxSize_->value()) * unitMultiplier(maxUnit_->currentIndex());
        if (maximum && minimum > maximum) {
            QMessageBox::warning(this, tr("筛选条件无效"), tr("最小文件大小不能大于最大文件大小。"));
            return;
        }
        if (afterEnabled_->isChecked() && beforeEnabled_->isChecked() && afterDate_->date() > beforeDate_->date()) {
            QMessageBox::warning(this, tr("筛选条件无效"), tr("开始日期不能晚于结束日期。"));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        pathEdit_->clear(); nameEdit_->clear(); extensionsEdit_->clear(); ownerEdit_->clear();
        entryType_->setCurrentIndex(0); minSize_->setValue(0); maxSize_->setValue(0);
        afterEnabled_->setChecked(false); beforeEnabled_->setChecked(false);
        temporary_->setChecked(false); cache_->setChecked(false); build_->setChecked(false); logs_->setChecked(false);
        includeRules_->clear(); excludeRules_->clear();
    });
    root->addWidget(buttons);
}

void FilterDialog::applyTypePreset(int index) {
    static const QStringList values = {"", ".doc,.docx,.pdf,.txt,.md,.xls,.xlsx,.ppt,.pptx",
        ".jpg,.jpeg,.png,.gif,.bmp,.webp", ".mp3,.wav,.flac,.aac", ".mp4,.mkv,.avi,.mov,.wmv",
        ".zip,.7z,.rar,.tar,.gz", ".c,.cpp,.h,.hpp,.java,.py,.js,.ts,.go,.rs,.cs"};
    if (index > 0 && index < values.size()) extensionsEdit_->setText(values[index]);
}

FilterOptions FilterDialog::options() const {
    FilterOptions result;
    result.pathContains = pathEdit_->text().trimmed().toUtf8().toStdString();
    result.nameContains = nameEdit_->text().trimmed().toUtf8().toStdString();
    result.ownerContains = ownerEdit_->text().trimmed().toUtf8().toStdString();
    result.type = static_cast<EntryTypeFilter>(entryType_->currentIndex());
    for (const QString &value : extensionsEdit_->text().split(',', Qt::SkipEmptyParts))
        result.extensions.push_back(value.trimmed().toUtf8().toStdString());
    result.minSize = static_cast<uint64_t>(minSize_->value()) * unitMultiplier(minUnit_->currentIndex());
    result.maxSize = static_cast<uint64_t>(maxSize_->value()) * unitMultiplier(maxUnit_->currentIndex());
    if (afterEnabled_->isChecked()) result.afterMtimeMs = QDateTime(afterDate_->date(), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    if (beforeEnabled_->isChecked()) result.beforeMtimeMs = QDateTime(beforeDate_->date(), QTime(23, 59, 59, 999), QTimeZone::UTC).toMSecsSinceEpoch();
    for (const QString &line : ruleLines(includeRules_)) result.pathRules.push_back({true, line.toUtf8().toStdString()});
    for (const QString &line : ruleLines(excludeRules_)) result.pathRules.push_back({false, line.toUtf8().toStdString()});
    auto exclude = [&](const char *pattern) { result.pathRules.push_back({false, pattern}); };
    if (temporary_->isChecked()) { exclude("*.tmp"); exclude("*.temp"); exclude("~*"); }
    if (cache_->isChecked()) { exclude("*Thumbs.db"); exclude("*desktop.ini"); }
    if (build_->isChecked()) { for (const char *dir : {"node_modules", "build", "out", "target"}) {
        exclude((std::string(dir) + "/*").c_str()); exclude((std::string("*/") + dir + "/*").c_str()); } }
    if (logs_->isChecked()) exclude("*.log");
    return result;
}
