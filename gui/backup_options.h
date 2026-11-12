// 备份方式、格式、压缩和加密选项界面
// 声明备份选项控件和取值接口；手动备份页面可隐藏模式选择，计划页面可启用增量模式。
#pragma once

#include "core/archive/archive.h"
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;

// 封装备份模式、打包格式、压缩、加密和密码输入控件。
class BackupOptionsWidget final : public QWidget {
  public:
    // 创建紧凑备份选项面板；allowIncremental=false 时只提供完整备份
    explicit BackupOptionsWidget(QWidget* parent = nullptr, bool allowIncremental = true);
    // 查询是否选择增量备份模式
    bool incremental() const;
    // 查询是否启用了备份加密
    bool encryptionEnabled() const;
    // 读取当前打包、压缩、密码和筛选设置
    PackOptions options() const;
    // 从已保存计划恢复备份方式与算法选择
    void setOptions(bool incremental, bool encrypted, PackFormat format, Compression compression);
    // 将已提供的密码填入备份选项
    void setPassword(const QString& password);
    // 实时计划必须使用增量备份模式
    void setIncrementalRequired(bool required);

  private:
    // 根据当前备份类型刷新可用控件
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
    bool allowIncremental_ = true;
    bool incrementalRequired_ = false;
    int previousMode_ = 0;
};
