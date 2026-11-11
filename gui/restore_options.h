#pragma once

#include <QCheckBox>

// 创建默认启用的设备节点恢复选项控件。
inline QCheckBox* createDeviceRestoreOption(QWidget* parent = nullptr) {
    auto* option = new QCheckBox(QObject::tr("恢复字符和块设备节点（可能需要管理员权限）"), parent);
    option->setChecked(true);
    return option;
}
