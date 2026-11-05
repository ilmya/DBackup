#include "core/archive.h"
#include "core/repository.h"
#include "core/credential_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
const char *kMarker = ".dbackup-acceptance.json";
const char *kTestPassword = "DBackup-Acceptance-Only!2026";

struct Arguments {
    QString command;
    QStringList rest;
    QStringList values(const QString &name) const {
        QStringList result;
        for (int i = 0; i + 1 < rest.size(); ++i)
            if (rest[i] == name) result.append(rest[i + 1]);
        return result;
    }
    QString value(const QString &name, const QString &fallback = {}) const {
        const auto found = values(name);
        return found.isEmpty() ? fallback : found.last();
    }
    bool has(const QString &name) const { return rest.contains(name); }
};

void emitResult(bool ok, const QString &command, const QJsonObject &data = {},
                const QString &error = {}) {
    QJsonObject result{{"ok", ok}, {"command", command}, {"data", data}};
    if (!error.isEmpty()) result["error"] = error;
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
}

bool readRoot(const QString &input, QString &root, QString &error) {
    const QFileInfo info(input);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        error = "测试根目录必须是已存在且非链接的目录";
        return false;
    }
    root = QDir::cleanPath(info.canonicalFilePath());
    QFile marker(QDir(root).filePath(QString::fromLatin1(kMarker)));
    if (!marker.open(QIODevice::ReadOnly)) {
        error = "测试根目录缺少有效验收标记";
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(marker.readAll(), &parse);
    const QJsonObject value = document.object();
    const QString recordedRoot = QDir::cleanPath(QFileInfo(value.value("root").toString()).absoluteFilePath());
    static const QRegularExpression uuidPattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    if (parse.error != QJsonParseError::NoError || value.value("kind").toString() != "dbackup-acceptance-root" ||
        value.value("version").toInt() != 1 || !uuidPattern.match(value.value("uuid").toString()).hasMatch() ||
        recordedRoot.compare(root, Qt::CaseSensitive) != 0) {
        error = "验收标记无效或与实际目录不匹配";
        return false;
    }
    return true;
}

bool insideRoot(const QString &root, const QString &path, QString &normalized, QString &error) {
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString relative = QDir(root).relativeFilePath(absolute);
    if (relative == "." || relative == ".." || relative.startsWith("../") ||
        QDir::isAbsolutePath(relative)) {
        error = "操作路径必须位于验收测试根目录内部";
        return false;
    }
    QString cursor = absolute;
    while (cursor.compare(root, Qt::CaseSensitive) != 0) {
        const QFileInfo component(cursor);
        if (component.isSymLink()) {
            error = "操作路径不能经过符号链接";
            return false;
        }
        const QString parent = QFileInfo(cursor).absolutePath();
        if (parent == cursor || parent.isEmpty()) break;
        cursor = parent;
    }
    normalized = absolute;
    return true;
}

bool getRootAndPath(const Arguments &args, const QString &option, QString &root,
                    QString &path, QString &error) {
    if (!readRoot(args.value("--root"), root, error)) return false;
    if (!insideRoot(root, args.value(option), path, error)) return false;
    return true;
}

bool getSources(const Arguments &args, const QString &root, std::vector<std::string> &sources,
                QString &error) {
    const QStringList values = args.values("--source");
    if (values.isEmpty()) { error = "至少需要一个 --source"; return false; }
    for (const QString &value : values) {
        QString path;
        if (!insideRoot(root, value, path, error) || !QFileInfo::exists(path)) {
            if (error.isEmpty()) error = "来源不存在";
            return false;
        }
        sources.push_back(path.toUtf8().toStdString());
    }
    return true;
}

bool parseFilter(const Arguments &args, FilterOptions &filter, QString &error) {
    if (!args.has("--filter")) return true;
    std::string detail;
    if (!ParseFilterOptions(args.value("--filter").toUtf8().toStdString(), filter, detail)) {
        error = QString::fromUtf8(detail.c_str());
        return false;
    }
    return true;
}

QString operationStage(OperationStage stage) {
    switch (stage) {
        case OperationStage::Scanning: return "scanning";
        case OperationStage::Reading: return "reading";
        case OperationStage::Compressing: return "compressing";
        case OperationStage::Encrypting: return "encrypting";
        case OperationStage::Writing: return "writing";
        case OperationStage::Uploading: return "uploading";
        case OperationStage::Downloading: return "downloading";
        case OperationStage::Restoring: return "restoring";
        case OperationStage::Completed: return "completed";
    }
    return "unknown";
}

OperationContext contextForProgress() {
    OperationContext context;
    context.onProgress = [](const OperationProgress &progress) {
        QJsonObject item{{"stage", operationStage(progress.stage)},
                         {"path", QString::fromUtf8(progress.currentPath.c_str())},
                         {"files", static_cast<qint64>(progress.completedFiles)},
                         {"bytes", static_cast<qint64>(progress.completedBytes)}};
        std::cerr << QJsonDocument(item).toJson(QJsonDocument::Compact).constData() << std::endl;
    };
    return context;
}

bool archiveCommand(const Arguments &args, QString &error, QJsonObject &data) {
    QString root;
    if (!readRoot(args.value("--root"), root, error)) return false;
    const QString operation = args.rest.value(0);
    if (operation == "preview" || operation == "create") {
        std::vector<std::string> sources;
        if (!getSources(args, root, sources, error)) return false;
        FilterOptions filter;
        if (!parseFilter(args, filter, error)) return false;
        if (operation == "preview") {
            BackupPreview preview;
            std::string detail;
            const bool ok = ArchiveService::preview(sources, filter, preview, detail);
            data = {{"files", static_cast<qint64>(preview.includedFiles)},
                    {"directories", static_cast<qint64>(preview.includedDirectories)},
                    {"bytes", static_cast<qint64>(preview.includedBytes)},
                    {"excludedFiles", static_cast<qint64>(preview.excludedFiles)},
                    {"excludedDirectories", static_cast<qint64>(preview.excludedDirectories)}};
            if (!ok) error = QString::fromUtf8(detail.c_str());
            else data["warning"] = QString::fromUtf8(detail.c_str());
            return ok;
        }
        QString output;
        if (!insideRoot(root, args.value("--output"), output, error)) return false;
        PackOptions options;
        options.filter = filter;
        const auto format = args.value("--format", "custom");
        if (format == "tar") options.format = PackFormat::Tar;
        else if (format != "custom") { error = "--format 必须为 custom 或 tar"; return false; }
        const auto algorithm = args.value("--algorithm", "deflate");
        if (algorithm == "none") options.compression = Compression::None;
        else if (algorithm == "huffman") options.compression = Compression::Huffman;
        else if (algorithm == "lz77") options.compression = Compression::Lz77;
        else if (algorithm == "deflate") options.compression = Compression::Deflate;
        else { error = "--algorithm 必须为 none、huffman、lz77 或 deflate"; return false; }
        bool compressionValid = false;
        options.compressionLevel = args.value("--compression", "6").toInt(&compressionValid);
        if (!compressionValid || options.compressionLevel < 0 || options.compressionLevel > 9) {
            error = "--compression 必须为 0 到 9";
            return false;
        }
        options.password = qEnvironmentVariable("DBACKUP_TEST_PASSWORD").toUtf8().toStdString();
        std::string detail;
        const bool ok = ArchiveService::create(sources, output.toUtf8().toStdString(), options,
                                               detail, contextForProgress());
        if (ok) data = {{"archive", output}, {"warning", QString::fromUtf8(detail.c_str())}};
        else error = QString::fromUtf8(detail.c_str());
        return ok;
    }
    QString input;
    if (!insideRoot(root, args.value("--input"), input, error)) return false;
    const QString password = qEnvironmentVariable("DBACKUP_TEST_PASSWORD");
    if (operation == "inspect") {
        std::vector<ArchiveEntry> entries;
        std::string detail;
        const bool ok = ArchiveService::inspect(input.toUtf8().toStdString(), entries, detail,
                                                password.toUtf8().toStdString());
        if (!ok) error = QString::fromUtf8(detail.c_str());
        QJsonArray result;
        for (const auto &entry : entries)
            result.append(QJsonObject{{"path", QString::fromUtf8(entry.relativePath.c_str())},
                                      {"type", entry.type}, {"size", static_cast<qint64>(entry.originalSize)}});
        data = {{"entries", result}, {"count", result.size()}};
        return ok;
    }
    if (operation == "extract") {
        QString destination;
        if (!insideRoot(root, args.value("--output"), destination, error)) return false;
        std::string detail;
        auto context = contextForProgress();
        context.allowDeviceNodes = !args.has("--skip-device-nodes");
        const bool ok = ArchiveService::extract(input.toUtf8().toStdString(), destination.toUtf8().toStdString(),
                                                detail, password.toUtf8().toStdString(), context);
        if (!ok) error = QString::fromUtf8(detail.c_str());
        else data = {{"destination", destination}, {"warning", QString::fromUtf8(detail.c_str())}};
        return ok;
    }
    error = "archive 子命令须为 preview、create、inspect 或 extract";
    return false;
}

bool repositoryCommand(const Arguments &args, QString &error, QJsonObject &data) {
    QString root, repositoryPath;
    if (!getRootAndPath(args, "--repository", root, repositoryPath, error)) return false;
    LocalRepository repository(repositoryPath.toUtf8().toStdString());
    std::string detail;
    const QString password = qEnvironmentVariable("DBACKUP_TEST_PASSWORD", QString::fromLatin1(kTestPassword));
    const QString operation = args.rest.value(0);
    if (!repository.initialize(password.toUtf8().toStdString(), detail)) {
        error = QString::fromUtf8(detail.c_str());
        return false;
    }
    if (operation == "init") { data = {{"repository", repositoryPath}, {"initialized", true}}; return true; }
    if (operation == "snapshot") {
        SnapshotOptions options;
        options.password = password.toUtf8().toStdString();
        if (!getSources(args, root, options.sources, error)) return false;
        if (!parseFilter(args, options.filter, error)) return false;
        SnapshotInfo snapshot;
        if (!repository.createSnapshot(options, snapshot, detail, contextForProgress())) {
            error = QString::fromUtf8(detail.c_str()); return false;
        }
        data = {{"id", QString::fromUtf8(snapshot.id.c_str())},
                {"partial", snapshot.partial}, {"warning", QString::fromUtf8(detail.c_str())},
                {"files", static_cast<qint64>(snapshot.fileCount)},
                {"bytes", static_cast<qint64>(snapshot.logicalBytes)},
                {"storedBytes", static_cast<qint64>(snapshot.storedBytes)}};
        return true;
    }
    if (operation == "list") {
        std::vector<SnapshotInfo> snapshots;
        if (!repository.listSnapshots(snapshots, detail)) { error = QString::fromUtf8(detail.c_str()); return false; }
        QJsonArray values;
        for (const auto &snapshot : snapshots)
            values.append(QJsonObject{{"id", QString::fromUtf8(snapshot.id.c_str())},
                                      {"createdAt", static_cast<qint64>(snapshot.createdAtMs)}});
        data = {{"snapshots", values}};
        return true;
    }
    if (operation == "restore") {
        QString destination;
        if (!insideRoot(root, args.value("--output"), destination, error)) return false;
        const bool ok = repository.restoreSnapshot(args.value("--id").toStdString(),
            destination.toUtf8().toStdString(), password.toUtf8().toStdString(), detail, contextForProgress());
        if (!ok) error = QString::fromUtf8(detail.c_str()); else data = {{"destination", destination}};
        return ok;
    }
    if (operation == "delete") {
        const bool ok = repository.deleteSnapshot(args.value("--id").toStdString(), detail);
        if (!ok) error = QString::fromUtf8(detail.c_str()); else data = {{"deleted", args.value("--id")}};
        return ok;
    }
    if (operation == "prune") {
        bool valid = false;
        const int keep = args.value("--keep").toInt(&valid);
        if (!valid || keep < 1 || keep > 1000) { error = "--keep 必须为 1 到 1000"; return false; }
        const bool ok = repository.prune(static_cast<size_t>(keep), detail);
        if (!ok) error = QString::fromUtf8(detail.c_str()); else data = {{"kept", keep}};
        return ok;
    }
    error = "repository 子命令须为 snapshot、list、restore、delete 或 prune";
    return false;
}


void printHelp() {
    std::cout << "BackupToolTestCli - development-only DBackup test driver\n"
        << "Commands (all file paths are restricted to a marked acceptance root):\n"
        << "  archive preview --root DIR --source PATH [--source PATH] [--filter SPEC]\n"
        << "  archive create  --root DIR --source PATH --output FILE [--format custom|tar] [--algorithm none|huffman|lz77|deflate] [--filter SPEC]\n"
        << "  archive inspect --root DIR --input FILE\n"
        << "  archive extract --root DIR --input FILE --output DIR [--skip-device-nodes]\n"
        << "  repository snapshot --root DIR --repository DIR --source PATH [--source PATH]\n"
        << "  repository init     --root DIR --repository DIR\n"
        << "  repository list --root DIR --repository DIR\n"
        << "  repository restore --root DIR --repository DIR --id ID --output DIR\n"
        << "  repository delete --root DIR --repository DIR --id ID\n"
        << "  repository prune --root DIR --repository DIR --keep N\n"
        << "Set DBACKUP_TEST_PASSWORD to override the test-only repository password.\n";
}
}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments[1] == "--help" || arguments[1] == "-h") {
        printHelp();
        return arguments.size() < 2 ? 2 : 0;
    }
    Arguments args;
    args.command = arguments[1];
    args.rest = arguments.mid(2);
    QString error;
    QJsonObject data;
    bool ok = false;
    if (args.command == "archive") ok = archiveCommand(args, error, data);
    else if (args.command == "repository") ok = repositoryCommand(args, error, data);
    else { error = "未知命令；使用 --help 查看用法"; }
    emitResult(ok, args.command, data, error);
    return ok ? 0 : (error.contains("验收标记") || error.contains("测试根目录") ? 4 : 1);
}
