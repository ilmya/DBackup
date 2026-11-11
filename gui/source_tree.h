#pragma once

#include "core/archive.h"
#include <QFileSystemModel>
#include <QMap>
#include <QTreeView>

class SourceTreeModel final : public QFileSystemModel {
  public:
    // 创建可勾选的本地文件系统树模型。
    explicit SourceTreeModel(QObject* parent = nullptr);
    // 提供文件名、简化大小和修改时间列的数据。
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    // 设置来源树列标题及对齐方式。
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    // 仅允许目录展开和勾选状态修改。
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    // 更新指定节点的包含状态，并同步父子规则。
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    // 返回当前来源树的规范化包含/排除规则。
    std::vector<SourceSelectionRule> rules() const;
    // 返回有效选择的最小来源根路径。
    QStringList roots() const;
    // 使用已保存规则恢复目录树的勾选状态。
    void setRules(const std::vector<SourceSelectionRule>& rules);

  private:
    // 按最深层来源规则判断节点是否选中。
    bool selected(const QString& path, bool includeSelf = true) const;
    QMap<QString, bool> rules_;
};

// 创建启用文件信息列、延迟加载和勾选选择的目录树控件。
QTreeView* createSourceTree(SourceTreeModel* model, QWidget* parent = nullptr);
