#include "gui/source_tree.h"
#include "core/backup_policy.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <memory>

namespace {
// 将字节数缩写为适合目录树窄列显示的单位。
QString compactSize(qint64 bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f',
                           unit && value < 10 && value != static_cast<qint64>(value) ? 1 : 0) +
           QLatin1String(units[unit]);
}
} // namespace

SourceTreeModel::SourceTreeModel(QObject* parent) : QFileSystemModel(parent) {
    setReadOnly(true);
    setOption(QFileSystemModel::DontResolveSymlinks, true);
    setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    setRootPath("/");
}

QVariant SourceTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case 0:
            return tr("名称");
        case 1:
            return tr("大小");
        case 3:
            return tr("日期");
        }
    }
    return QFileSystemModel::headerData(section, orientation, role);
}

bool SourceTreeModel::selected(const QString& path, bool includeSelf) const {
    return dbackup::backup_policy::sourcePathSelected(
        std::filesystem::path(path.toUtf8().toStdString()), rules(), includeSelf, false);
}

QVariant SourceTreeModel::data(const QModelIndex& index, int role) const {
    if (index.isValid() && (index.column() == 1 || index.column() == 3) &&
        (role == Qt::DisplayRole || role == Qt::ToolTipRole)) {
        const QFileInfo info = fileInfo(index);
        if (index.column() == 1) {
            if (!info.isFile() || info.isSymLink())
                return role == Qt::DisplayRole ? QVariant(QStringLiteral("—")) : QVariant();
            return role == Qt::DisplayRole ? QVariant(compactSize(info.size()))
                                           : QVariant(tr("%1 字节").arg(info.size()));
        }
        return info.lastModified().toString(role == Qt::DisplayRole ? "yyyy-MM-dd"
                                                                    : "yyyy-MM-dd HH:mm:ss");
    }
    if (role != Qt::CheckStateRole || !index.isValid() || index.column() != 0)
        return QFileSystemModel::data(index, role);
    const QString path = QDir::cleanPath(filePath(index));
    const bool current = selected(path);
    if (dbackup::backup_policy::hasMixedSourceSelection(
            std::filesystem::path(path.toUtf8().toStdString()), rules(), false))
        return Qt::PartiallyChecked;
    return current ? Qt::Checked : Qt::Unchecked;
}

Qt::ItemFlags SourceTreeModel::flags(const QModelIndex& index) const {
    auto result = QFileSystemModel::flags(index);
    return index.isValid() && index.column() == 0 ? result | Qt::ItemIsUserCheckable : result;
}

bool SourceTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::CheckStateRole || !index.isValid() || index.column() != 0)
        return false;
    const QString path = QDir::cleanPath(filePath(index));
    const bool checked = value.toInt() == Qt::Checked;
    const auto updated = dbackup::backup_policy::toggleSourceSelection(
        std::filesystem::path(path.toUtf8().toStdString()), checked, rules(), false);
    rules_.clear();
    for (const auto& rule : updated)
        rules_.insert(QDir::cleanPath(QString::fromUtf8(rule.path.c_str())), rule.include);
    emit layoutChanged();
    return true;
}

std::vector<SourceSelectionRule> SourceTreeModel::rules() const {
    std::vector<SourceSelectionRule> result;
    for (auto it = rules_.cbegin(); it != rules_.cend(); ++it)
        result.push_back({it.key().toUtf8().toStdString(), it.value()});
    return result;
}

void SourceTreeModel::setRules(const std::vector<SourceSelectionRule>& rules) {
    rules_.clear();
    for (const auto& rule : rules)
        rules_.insert(QDir::cleanPath(QString::fromUtf8(rule.path.c_str())), rule.include);
    emit layoutChanged();
}

QStringList SourceTreeModel::roots() const {
    QStringList result;
    for (const auto& root : dbackup::backup_policy::selectedSourceRoots(rules()))
        result.push_back(QString::fromUtf8(root.c_str()));
    return result;
}

QTreeView* createSourceTree(SourceTreeModel* model, QWidget* parent) {
    auto* tree = new QTreeView(parent);
    tree->setModel(model);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setRootIndex(model->index("/"));
    tree->setHeaderHidden(true);
    for (int column = 1; column < model->columnCount(); ++column)
        tree->hideColumn(column);
    tree->setMinimumWidth(230);
    const QString home = QDir::homePath();
    QString path = "/";
    for (const auto& part : home.split('/', Qt::SkipEmptyParts)) {
        path = QDir(path).filePath(part);
        const QModelIndex index = model->index(path);
        if (index.isValid())
            tree->expand(index);
    }
    auto positioned = std::make_shared<bool>(false);
    QObject::connect(model, &QFileSystemModel::directoryLoaded, tree,
                     [tree, model, home, positioned](const QString& loaded) {
                         if (*positioned ||
                             QDir::cleanPath(loaded) != QFileInfo(home).absolutePath())
                             return;
                         const QModelIndex index = model->index(home);
                         if (!index.isValid())
                             return;
                         tree->setCurrentIndex(index);
                         tree->scrollTo(index);
                         *positioned = true;
                     });
    return tree;
}
