#pragma once

#include "core/archive.h"
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;

class BackupOptionsWidget final : public QWidget {
  public:
    // 创建紧凑备份选项面板；allowRepository=false 时只提供独立归档。
    explicit BackupOptionsWidget(QWidget* parent = nullptr, bool allowRepository = true);
    // 查询当前是否使用增量版本仓库。
    bool incremental() const;
    bool tarIncremental() const;
    // 查询是否启用了归档加密。
    bool encryptionEnabled() const;
    // 读取当前打包、压缩、密码和筛选设置。
    PackOptions options() const;
    // 从已保存计划恢复备份方式与算法选择。
    void setOptions(bool repository, bool encrypted, PackFormat format, Compression compression,
                    bool tarIncremental = false);
    void setPassword(const QString& password);
    // 实时计划必须使用增量模式。
    void setRepositoryRequired(bool required);

  private:
    // 根据当前备份类型刷新可用控件。
    void updateEnabled();
    QButtonGroup* mode_ = nullptr;
    QButtonGroup* format_ = nullptr;
    QButtonGroup* compression_ = nullptr;
    QWidget* formatChoices_ = nullptr;
    QWidget* compressionChoices_ = nullptr;
    QCheckBox* compress_ = nullptr;
    QCheckBox* encrypt_ = nullptr;
    QCheckBox* showPassword_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLabel* repositoryHint_ = nullptr;
    QLabel* repositoryEncryption_ = nullptr;
    bool allowRepository_ = true;
    bool repositoryRequired_ = false;
    int previousMode_ = 0;
};
