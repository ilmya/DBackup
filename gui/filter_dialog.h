#pragma once

#include <QDialog>
#include "packer.h"

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

class FilterDialog final : public QDialog {
 public:
    explicit FilterDialog(const FilterOptions &options, QWidget *parent = nullptr);
    FilterOptions options() const;

 private:
    void applyTypePreset(int index);

    QLineEdit *pathEdit_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *extensionsEdit_ = nullptr;
    QLineEdit *ownerEdit_ = nullptr;
    QComboBox *entryType_ = nullptr;
    QSpinBox *minSize_ = nullptr;
    QSpinBox *maxSize_ = nullptr;
    QComboBox *minUnit_ = nullptr;
    QComboBox *maxUnit_ = nullptr;
    QCheckBox *afterEnabled_ = nullptr;
    QCheckBox *beforeEnabled_ = nullptr;
    QDateEdit *afterDate_ = nullptr;
    QDateEdit *beforeDate_ = nullptr;
    QCheckBox *temporary_ = nullptr;
    QCheckBox *cache_ = nullptr;
    QCheckBox *build_ = nullptr;
    QCheckBox *logs_ = nullptr;
    QPlainTextEdit *includeRules_ = nullptr;
    QPlainTextEdit *excludeRules_ = nullptr;
};
