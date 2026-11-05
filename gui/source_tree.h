#pragma once

#include "core/archive.h"
#include <QFileSystemModel>
#include <QTreeView>
#include <QMap>

class SourceTreeModel final : public QFileSystemModel {
 public:
    explicit SourceTreeModel(QObject *parent = nullptr);
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    std::vector<SourceSelectionRule> rules() const;
    QStringList roots() const;
    void setRules(const std::vector<SourceSelectionRule> &rules);
 private:
    bool selected(const QString &path, bool includeSelf = true) const;
    QMap<QString, bool> rules_;
};

QTreeView *createSourceTree(SourceTreeModel *model, QWidget *parent = nullptr);
