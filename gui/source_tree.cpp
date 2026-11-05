#include "gui/source_tree.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <memory>

namespace {
bool under(const QString &path, const QString &parent) {
    return path == parent || path.startsWith(parent == "/" ? parent : parent + "/");
}
}

SourceTreeModel::SourceTreeModel(QObject *parent) : QFileSystemModel(parent) {
    setReadOnly(true);
    setOption(QFileSystemModel::DontResolveSymlinks, true);
    setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    setRootPath("/");
}

bool SourceTreeModel::selected(const QString &path, bool includeSelf) const {
    bool result = false;
    int length = -1;
    for (auto it = rules_.cbegin(); it != rules_.cend(); ++it) {
        if ((!includeSelf && it.key() == path) || !under(path, it.key())) continue;
        if (it.key().size() > length) { length = it.key().size(); result = it.value(); }
    }
    return result;
}

QVariant SourceTreeModel::data(const QModelIndex &index, int role) const {
    if (role != Qt::CheckStateRole || !index.isValid() || index.column() != 0)
        return QFileSystemModel::data(index, role);
    const QString path = QDir::cleanPath(filePath(index));
    const bool current = selected(path);
    for (auto it = rules_.cbegin(); it != rules_.cend(); ++it) {
        if (it.key() != path && under(it.key(), path) && it.value() != current)
            return Qt::PartiallyChecked;
    }
    return current ? Qt::Checked : Qt::Unchecked;
}

Qt::ItemFlags SourceTreeModel::flags(const QModelIndex &index) const {
    auto result = QFileSystemModel::flags(index);
    return index.isValid() && index.column() == 0 ? result | Qt::ItemIsUserCheckable : result;
}

bool SourceTreeModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::CheckStateRole || !index.isValid() || index.column() != 0) return false;
    const QString path = QDir::cleanPath(filePath(index));
    const bool checked = value.toInt() == Qt::Checked;
    for (auto it = rules_.begin(); it != rules_.end(); ) {
        if (under(it.key(), path)) it = rules_.erase(it); else ++it;
    }
    if (selected(path, false) != checked) rules_.insert(path, checked);
    emit layoutChanged();
    return true;
}

std::vector<SourceSelectionRule> SourceTreeModel::rules() const {
    std::vector<SourceSelectionRule> result;
    for (auto it = rules_.cbegin(); it != rules_.cend(); ++it)
        result.push_back({it.key().toUtf8().toStdString(), it.value()});
    return result;
}

void SourceTreeModel::setRules(const std::vector<SourceSelectionRule> &rules) {
    rules_.clear();
    for (const auto &rule : rules) rules_.insert(QDir::cleanPath(QString::fromUtf8(rule.path.c_str())), rule.include);
    emit layoutChanged();
}

QStringList SourceTreeModel::roots() const {
    QStringList result;
    for (auto it = rules_.cbegin(); it != rules_.cend(); ++it) {
        if (!it.value()) continue;
        bool covered = false;
        for (const auto &root : result) if (under(it.key(), root)) { covered = true; break; }
        if (!covered) result.push_back(it.key());
    }
    return result;
}

QTreeView *createSourceTree(SourceTreeModel *model, QWidget *parent) {
    auto *tree = new QTreeView(parent);
    tree->setModel(model);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setRootIndex(model->index("/"));
    tree->setHeaderHidden(true);
    for (int column = 1; column < model->columnCount(); ++column) tree->hideColumn(column);
    tree->setMinimumWidth(230);
    const QString home = QDir::homePath();
    QString path = "/";
    for (const auto &part : home.split('/', Qt::SkipEmptyParts)) {
        path = QDir(path).filePath(part);
        const QModelIndex index = model->index(path);
        if (index.isValid()) tree->expand(index);
    }
    auto positioned = std::make_shared<bool>(false);
    QObject::connect(model, &QFileSystemModel::directoryLoaded, tree, [tree, model, home, positioned](const QString &loaded) {
        if (*positioned || QDir::cleanPath(loaded) != QFileInfo(home).absolutePath()) return;
        const QModelIndex index = model->index(home);
        if (!index.isValid()) return;
        tree->setCurrentIndex(index);
        tree->scrollTo(index);
        *positioned = true;
    });
    return tree;
}
