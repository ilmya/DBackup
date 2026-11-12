// 备份筛选条件编辑界面
// 声明筛选编辑对话框，保存输入控件并向调用方返回 FilterOptions。
#pragma once

#include "core/archive/archive.h"
#include <QDialog>
#include <array>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

// 编辑筛选条件并在确认后向调用方提供结构化配置。
class FilterDialog final : public QDialog {
  public:
    // 按已有筛选条件初始化可编辑的筛选对话框
    explicit FilterDialog(const FilterOptions& options, QWidget* parent = nullptr);
    // 从界面控件收集并返回筛选条件
    FilterOptions options() const;

  private:
    // 按预设切换常见文件类型勾选项
    void applyTypePreset(int index);

    QLineEdit* pathEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* extensionsEdit_ = nullptr;
    QLineEdit* ownerEdit_ = nullptr;
    QComboBox* entryType_ = nullptr;
    QSpinBox* minSize_ = nullptr;
    QSpinBox* maxSize_ = nullptr;
    QComboBox* minUnit_ = nullptr;
    QComboBox* maxUnit_ = nullptr;
    QCheckBox* afterEnabled_ = nullptr;
    QCheckBox* beforeEnabled_ = nullptr;
    QDateEdit* afterDate_ = nullptr;
    QDateEdit* beforeDate_ = nullptr;
    QCheckBox* temporary_ = nullptr;
    QCheckBox* build_ = nullptr;
    QCheckBox* logs_ = nullptr;
    std::array<QCheckBox*, 8> fileTypes_{};
    QPlainTextEdit* includeRules_ = nullptr;
    QPlainTextEdit* excludeRules_ = nullptr;
};
