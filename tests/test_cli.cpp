#include "archive/archive.h"
#include "network/remote_client.h"
#include "network/remote_repository_service.h"
#include "repository/repository.h"
#include "security/credential_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
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
        recordedRoot.compare(root, Qt::CaseInsensitive) != 0) {
        error = "验收标记无效或与实际目录不匹配";
        return false;
    }
    return true;
}

bool insideRoot(const QString &root, const QString &path, QString &normalized, QString &error) {
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString relative = QDir(root).relativeFilePath(absolute);
    if (relative == "." || relative == ".." || relative.startsWith("../") ||
        relative.startsWith("..\\") || QDir::isAbsolutePath(relative)) {
        error = "操作路径必须位于验收测试根目录内部";
        return false;
    }
    QString cursor = absolute;
    while (cursor.compare(root, Qt::CaseInsensitive) != 0) {
        const QFileInfo component(cursor);
        if (component.exists() && component.isSymLink()) {
            error = "操作路径不能经过符号链接或目录联接";
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
            return ok;
        }
        QString output;
        if (!insideRoot(root, args.value("--output"), output, error)) return false;
        PackOptions options;
        options.filter = filter;
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
        if (ok) data = {{"archive", output}};
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
        const bool ok = ArchiveService::extract(input.toUtf8().toStdString(), destination.toUtf8().toStdString(),
                                                detail, password.toUtf8().toStdString(), contextForProgress());
        if (!ok) error = QString::fromUtf8(detail.c_str());
        else data = {{"destination", destination}};
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

template <typename Starter>
bool awaitBool(Starter starter, QString &error, int timeoutMs = 15000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool finished = false, success = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    starter([&](bool ok, QString message) { finished = true; success = ok; error = message; loop.quit(); });
    if (!finished) { timer.start(timeoutMs); loop.exec(); }
    if (!finished && error.isEmpty()) error = "网络操作超时";
    return finished && success;
}

template <typename Starter, typename Value>
bool awaitValue(Starter starter, Value &value, QString &error, int timeoutMs = 15000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool finished = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    starter([&](Value result, QString message) { finished = true; value = std::move(result); error = message; loop.quit(); });
    if (!finished) { timer.start(timeoutMs); loop.exec(); }
    if (!finished && error.isEmpty()) error = "网络操作超时";
    return finished && error.isEmpty();
}

template <typename Starter>
bool awaitCompletion(Starter starter, QString &error, int timeoutMs = 30000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool finished = false, success = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    starter([&](bool ok, QString message) { finished = true; success = ok; error = message; loop.quit(); });
    if (!finished) { timer.start(timeoutMs); loop.exec(); }
    if (!finished && error.isEmpty()) error = "网络仓库操作超时";
    return finished && success;
}

QJsonObject restoredTreeMap(const QString &root) {
    QJsonObject files;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        const QFileInfo info(filePath);
        const QString relative = QDir(root).relativeFilePath(filePath).replace('\\', '/');
        if (info.isDir() && !info.isSymLink()) {
            files[relative] = "directory";
            continue;
        }
        if (info.isSymLink()) {
            files[relative] = "link:" + info.symLinkTarget();
            continue;
        }
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) return {};
        files[relative] = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
    }
    return files;
}

QJsonObject sourceTreeMap(const QStringList &sources) {
    QJsonObject expected;
    for (const QString &source : sources) {
        const QFileInfo info(source);
        const QString rootName = info.fileName();
        if (info.isDir()) {
            expected[rootName] = "directory";
            QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                const QFileInfo entry(path);
                const QString relative = rootName + "/" + QDir(source).relativeFilePath(path).replace('\\', '/');
                if (entry.isDir() && !entry.isSymLink()) { expected[relative] = "directory"; continue; }
                if (entry.isSymLink()) { expected[relative] = "link:" + entry.symLinkTarget(); continue; }
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) return {};
                expected[relative] = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
            }
        } else {
            QFile file(source);
            if (!file.open(QIODevice::ReadOnly)) return {};
            expected[rootName] = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
        }
    }
    return expected;
}

bool remoteClientTest(const Arguments &args, QString &error, QJsonObject &data) {
    QString root;
    if (!readRoot(args.value("--root"), root, error)) return false;
    const QUrl endpoint(args.value("--server"));
    const QString host = endpoint.host().toLower();
    if (endpoint.scheme() != "https" || (host != "127.0.0.1" && host != "localhost" && host != "::1") ||
        endpoint.port() <= 0) {
        error = "自动联测只允许使用 HTTPS 本机回环地址";
        return false;
    }
    const QString source = QDir(root).filePath("fixtures/network/alice");
    if (!QFileInfo(source).isDir()) { error = "请先运行 acceptance.sh generate 准备 fixtures/network/alice"; return false; }

    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString credential = "DBackupAcceptance/" + runId;
    const QString username = "acceptance-" + runId.left(12);
    const QString userPassword = "Acceptance-" + runId.left(16) + "!";
    RemoteClient client;
    client.configure(endpoint, credential);
    client.setCertificatePrompt([](QString, QString) { return true; });
    RemoteClient secondUser;
    secondUser.configure(endpoint, credential + "/second");
    secondUser.setCertificatePrompt([](QString, QString) { return true; });

    bool success = false;
    bool loggedIn = false;
    bool secondLoggedIn = false;
    do {
        if (!awaitBool([&](auto done) { client.registerUser(username, userPassword, done); }, error)) break;
        if (!awaitBool([&](auto done) { client.login(username, userPassword, done); }, error)) break;
        loggedIn = true;
        QJsonObject status;
        if (!awaitValue([&](auto done) { client.status(done); }, status, error)) break;

        const QString work = QDir(root).filePath("server-data/client-" + runId);
        if (!QDir().mkpath(work)) { error = "无法创建隔离远程测试目录"; break; }
        const QString localRepositoryPath = QDir(work).filePath("local-repository");
        LocalRepository local(localRepositoryPath.toUtf8().toStdString());
        std::string detail;
        const QString repoPassword = qEnvironmentVariable("DBACKUP_TEST_PASSWORD", QString::fromLatin1(kTestPassword));
        if (!local.initialize(repoPassword.toStdString(), detail)) { error = QString::fromUtf8(detail.c_str()); break; }
        SnapshotOptions options;
        options.sources.push_back(source.toUtf8().toStdString());
        options.password = repoPassword.toStdString();
        SnapshotInfo snapshot;
        if (!local.createSnapshot(options, snapshot, detail)) { error = QString::fromUtf8(detail.c_str()); break; }

        RemoteRepositoryService remote(&client);
        QStringList hashes;
        QDirIterator blocks(QDir(localRepositoryPath).filePath("chunks"), {"*.blk"}, QDir::Files, QDirIterator::Subdirectories);
        while (blocks.hasNext()) hashes.append(QFileInfo(blocks.next()).completeBaseName());
        hashes.removeDuplicates();
        if (hashes.size() < 2) { error = "远程续传测试至少需要两个不同的数据块"; break; }

        std::atomic_bool cancelUpload{false};
        bool uploadProgressSeen = false;
        const bool canceledUploadSucceeded = awaitCompletion([&](auto done) {
            remote.uploadSnapshot(localRepositoryPath, QString::fromStdString(snapshot.id), &cancelUpload,
                [&](int, QString) { uploadProgressSeen = true; cancelUpload.store(true); }, done);
        }, error);
        if (canceledUploadSucceeded || !uploadProgressSeen ||
            !(error.contains("取消") || error.contains("cancel", Qt::CaseInsensitive))) {
            if (error.isEmpty()) error = "上传取消没有在首个数据块后生效";
            break;
        }
        error.clear();
        QJsonArray beforeResume;
        if (!awaitValue([&](auto done) { client.listSnapshots(done); }, beforeResume, error)) break;
        if (!beforeResume.isEmpty()) { error = "取消的上传错误地提交了不完整快照"; break; }
        QStringList remaining;
        if (!awaitValue([&](auto done) { client.checkChunks(hashes, done); }, remaining, error)) break;
        if (remaining.isEmpty() || remaining.size() >= hashes.size()) {
            error = "取消上传后服务器端块状态不符合续传预期";
            break;
        }
        if (!awaitCompletion([&](auto done) {
                remote.uploadSnapshot(localRepositoryPath, QString::fromStdString(snapshot.id), nullptr, {}, done);
            }, error)) break;
        QJsonArray afterResume;
        if (!awaitValue([&](auto done) { client.listSnapshots(done); }, afterResume, error)) break;
        if (afterResume.size() != 1) { error = "续传后服务器快照数量不为 1"; break; }

        if (!awaitBool([&](auto done) { secondUser.registerUser("second-" + runId.left(12), userPassword, done); }, error)) break;
        if (!awaitBool([&](auto done) { secondUser.login("second-" + runId.left(12), userPassword, done); }, error)) break;
        secondLoggedIn = true;
        QJsonArray privateSnapshots;
        if (!awaitValue([&](auto done) { secondUser.listSnapshots(done); }, privateSnapshots, error)) break;
        if (!privateSnapshots.isEmpty()) { error = "用户隔离失败：第二个账号看到了第一个账号的快照"; break; }
        QJsonObject inaccessibleSnapshot;
        QString accessError;
        const bool canReadPrivateSnapshot = awaitValue([&](auto done) {
            secondUser.getSnapshot(QString::fromStdString(snapshot.id), done);
        }, inaccessibleSnapshot, accessError);
        if (canReadPrivateSnapshot) { error = "用户隔离失败：第二个账号读取到了第一个账号的快照"; break; }
        error.clear();
        QStringList missing;
        if (!awaitValue([&](auto done) { secondUser.checkChunks(hashes, done); }, missing, error)) break;
        if (missing.size() != hashes.size()) { error = "用户隔离失败：第二个账号探测到第一个账号的数据块"; break; }

        const QString cachePath = QDir(work).filePath("download-cache");
        QDir().mkpath(cachePath);
        std::atomic_bool cancelDownload{false};
        bool downloadProgressSeen = false;
        const bool canceledDownloadSucceeded = awaitCompletion([&](auto done) {
            remote.downloadSnapshot(QString::fromStdString(snapshot.id), cachePath, &cancelDownload,
                [&](int, QString) { downloadProgressSeen = true; cancelDownload.store(true); }, done);
        }, error);
        if (canceledDownloadSucceeded || !downloadProgressSeen ||
            !(error.contains("取消") || error.contains("cancel", Qt::CaseInsensitive))) {
            if (error.isEmpty()) error = "下载取消没有在首个数据块后生效";
            break;
        }
        error.clear();
        if (!awaitCompletion([&](auto done) {
                remote.downloadSnapshot(QString::fromStdString(snapshot.id), cachePath, nullptr, {}, done);
            }, error)) break;
        LocalRepository downloaded(cachePath.toUtf8().toStdString());
        if (!downloaded.initialize(repoPassword.toStdString(), detail)) { error = QString::fromUtf8(detail.c_str()); break; }
        const QString restored = QDir(work).filePath("restored");
        QDir().mkpath(restored);
        if (!downloaded.restoreSnapshot(snapshot.id, restored.toUtf8().toStdString(), repoPassword.toStdString(), detail)) {
            error = QString::fromUtf8(detail.c_str()); break;
        }
        if (sourceTreeMap({source}) != restoredTreeMap(restored)) { error = "远程恢复后的路径或 SHA-256 与来源不一致"; break; }
        if (!awaitBool([&](auto done) { client.deleteSnapshot(QString::fromStdString(snapshot.id), done); }, error)) break;
        if (!awaitBool([&](auto done) { client.logout(done); }, error)) break;
        loggedIn = false;
        if (!awaitBool([&](auto done) { secondUser.logout(done); }, error)) break;
        secondLoggedIn = false;
        data = {{"endpoint", endpoint.toString()}, {"snapshotId", QString::fromStdString(snapshot.id)},
                {"distinctBlocks", hashes.size()}, {"uploadResume", true}, {"downloadResume", true},
                {"isolated", true}, {"restoreHashesMatch", true}, {"deleted", true}};
        success = true;
    } while (false);

    QString cleanupError;
    if (loggedIn) awaitBool([&](auto done) { client.logout(done); }, cleanupError);
    if (secondLoggedIn) awaitBool([&](auto done) { secondUser.logout(done); }, cleanupError);
    std::string credentialError;
    CredentialStore::remove(credential.toStdString(), credentialError);
    CredentialStore::remove((credential + "/certificate").toStdString(), credentialError);
    CredentialStore::remove((credential + "/second").toStdString(), credentialError);
    CredentialStore::remove((credential + "/second/certificate").toStdString(), credentialError);
    if (!success && error.isEmpty()) error = "远程端到端测试失败";
    return success;
}

void printHelp() {
    std::cout << "BackupToolTestCli - development-only DBackup test driver\n"
        << "Commands (all file paths are restricted to a marked acceptance root):\n"
        << "  archive preview --root DIR --source PATH [--source PATH] [--filter SPEC]\n"
        << "  archive create  --root DIR --source PATH --output FILE [--compression 0..9] [--filter SPEC]\n"
        << "  archive inspect --root DIR --input FILE\n"
        << "  archive extract --root DIR --input FILE --output DIR\n"
        << "  repository snapshot --root DIR --repository DIR --source PATH [--source PATH]\n"
        << "  repository init     --root DIR --repository DIR\n"
        << "  repository list --root DIR --repository DIR\n"
        << "  repository restore --root DIR --repository DIR --id ID --output DIR\n"
        << "  repository delete --root DIR --repository DIR --id ID\n"
        << "  repository prune --root DIR --repository DIR --keep N\n"
        << "  remote-client-test --root DIR --server https://127.0.0.1:PORT\n"
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
    else if (args.command == "remote-client-test") ok = remoteClientTest(args, error, data);
    else { error = "未知命令；使用 --help 查看用法"; }
    emitResult(ok, args.command, data, error);
    return ok ? 0 : (error.contains("验收标记") || error.contains("测试根目录") ? 4 : 1);
}
