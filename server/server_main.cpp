#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>

namespace {
QByteArray randomBytes(int count){QByteArray value(count,Qt::Uninitialized);for(int i=0;i<count;i+=4){quint32 r=QRandomGenerator::system()->generate();memcpy(value.data()+i,&r,qMin(4,count-i));}return value;}
QByteArray hash(const QByteArray&value){return QCryptographicHash::hash(value,QCryptographicHash::Sha256);}
QHttpServerResponse json(const QJsonObject&o,QHttpServerResponse::StatusCode status=QHttpServerResponse::StatusCode::Ok){return QHttpServerResponse("application/json",QJsonDocument(o).toJson(QJsonDocument::Compact),status);}
bool validHash(const QString&s){if(s.size()!=64)return false;for(QChar c:s)if(!c.isDigit()&&(c.toLower()<'a'||c.toLower()>'f'))return false;return true;}

class Store {
 public:
    bool open(const QString&root,QString&error){root_=root;QDir().mkpath(root+"/data");db_=QSqlDatabase::addDatabase("QSQLITE","server");db_.setDatabaseName(root+"/server.sqlite");db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");if(!db_.open()){error=db_.lastError().text();return false;}QSqlQuery q(db_);QStringList schema={
        "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL, salt BLOB NOT NULL, password_hash BLOB NOT NULL, created_ms INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS tokens(token_hash BLOB PRIMARY KEY, user_id INTEGER NOT NULL, expires_ms INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS chunks(user_id INTEGER NOT NULL, hash TEXT NOT NULL, size INTEGER NOT NULL, ref_count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(user_id,hash))",
        "CREATE TABLE IF NOT EXISTS snapshots(id TEXT PRIMARY KEY, user_id INTEGER NOT NULL, created_ms INTEGER NOT NULL, name TEXT, manifest BLOB NOT NULL)",
        "CREATE TABLE IF NOT EXISTS snapshot_chunks(snapshot_id TEXT NOT NULL, hash TEXT NOT NULL, PRIMARY KEY(snapshot_id,hash))"};
        for(const auto&s:schema)if(!q.exec(s)){error=q.lastError().text();return false;}
        q.exec("DELETE FROM tokens WHERE expires_ms <= " + QString::number(QDateTime::currentMSecsSinceEpoch()));
        QDirIterator temporary(root,QDir::Files,QDirIterator::Subdirectories);while(temporary.hasNext()){const QString path=temporary.next();if((path.endsWith(".tmp")||path.endsWith(".part"))&&QFileInfo(path).lastModified().secsTo(QDateTime::currentDateTime())>86400)QFile::remove(path);}return true;}
    qint64 authenticate(const QHttpServerRequest&r){QByteArray auth=r.value("authorization");if(!auth.startsWith("Bearer "))return -1;QSqlQuery q(db_);q.prepare("SELECT user_id FROM tokens WHERE token_hash=? AND expires_ms>?");q.addBindValue(hash(auth.mid(7)));q.addBindValue(QDateTime::currentMSecsSinceEpoch());return q.exec()&&q.next()?q.value(0).toLongLong():-1;}
    bool registerUser(const QString&name,const QString&password,QString&error){if(name.size()<3||name.size()>64||password.size()<8){error="用户名至少 3 个字符，密码至少 8 个字符";return false;}QByteArray salt=randomBytes(16);QByteArray derived=QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,password.toUtf8(),salt,100000,32);QSqlQuery q(db_);q.prepare("INSERT INTO users(username,salt,password_hash,created_ms) VALUES(?,?,?,?)");q.addBindValue(name);q.addBindValue(salt);q.addBindValue(derived);q.addBindValue(QDateTime::currentMSecsSinceEpoch());if(!q.exec()){error="用户名已存在或数据库错误";return false;}return true;}
    QByteArray login(const QString&name,const QString&password){QSqlQuery q(db_);q.prepare("SELECT id,salt,password_hash FROM users WHERE username=?");q.addBindValue(name);if(!q.exec()||!q.next())return{};QByteArray actual=QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,password.toUtf8(),q.value(1).toByteArray(),100000,32);if(actual!=q.value(2).toByteArray())return{};QByteArray token=randomBytes(32).toHex();QSqlQuery insert(db_);insert.prepare("INSERT INTO tokens(token_hash,user_id,expires_ms) VALUES(?,?,?)");insert.addBindValue(hash(token));insert.addBindValue(q.value(0));insert.addBindValue(QDateTime::currentMSecsSinceEpoch()+24LL*60*60*1000);return insert.exec()?token:QByteArray{};}
    void logout(const QHttpServerRequest&r){QByteArray auth=r.value("authorization");if(auth.startsWith("Bearer ")){QSqlQuery q(db_);q.prepare("DELETE FROM tokens WHERE token_hash=?");q.addBindValue(hash(auth.mid(7)));q.exec();}}
    QString chunkPath(qint64 user,const QString&id)const{return root_+"/data/"+QString::number(user)+"/chunks/"+id.left(2)+"/"+id+".blk";}
    bool hasChunk(qint64 user,const QString&id){QSqlQuery q(db_);q.prepare("SELECT 1 FROM chunks WHERE user_id=? AND hash=?");q.addBindValue(user);q.addBindValue(id);return q.exec()&&q.next();}
    bool putChunk(qint64 user,const QString&id,const QByteArray&body,QString&error){if(body.size()>8*1024*1024){error="数据块超过 8 MiB 限制";return false;}QString path=chunkPath(user,id);QDir().mkpath(QFileInfo(path).path());QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(body)!=body.size()||!file.commit()){error="数据块写入失败";return false;}QSqlQuery q(db_);q.prepare("INSERT OR IGNORE INTO chunks(user_id,hash,size,ref_count) VALUES(?,?,?,0)");q.addBindValue(user);q.addBindValue(id);q.addBindValue(body.size());return q.exec();}
    QByteArray getChunk(qint64 user,const QString&id,bool&ok){ok=false;if(!hasChunk(user,id))return{};QFile f(chunkPath(user,id));if(!f.open(QIODevice::ReadOnly))return{};ok=true;return f.readAll();}
    QJsonArray snapshots(qint64 user){QJsonArray out;QSqlQuery q(db_);q.prepare("SELECT id,created_ms,name FROM snapshots WHERE user_id=? ORDER BY created_ms DESC LIMIT 200");q.addBindValue(user);if(q.exec())while(q.next())out.append(QJsonObject{{"id",q.value(0).toString()},{"createdAt",q.value(1).toDouble()},{"name",q.value(2).toString()}});return out;}
    QJsonObject status(qint64 user){QSqlQuery q(db_);q.prepare("SELECT COUNT(*),COALESCE(SUM(size),0) FROM chunks WHERE user_id=?");q.addBindValue(user);q.exec();q.next();const auto chunkCount=q.value(0).toDouble(),stored=q.value(1).toDouble();QSqlQuery snapshots(db_);snapshots.prepare("SELECT COUNT(*) FROM snapshots WHERE user_id=?");snapshots.addBindValue(user);snapshots.exec();snapshots.next();return {{"connected",true},{"snapshotCount",snapshots.value(0).toDouble()},{"chunkCount",chunkCount},{"storedBytes",stored}};}
    bool commitSnapshot(qint64 user,const QJsonObject&o,QString&error){QString id=o["id"].toString();QJsonArray chunks=o["chunks"].toArray();if(id.isEmpty()||chunks.size()>1000000){error="快照清单无效";return false;}for(auto v:chunks)if(!validHash(v.toString())||!hasChunk(user,v.toString())){error="快照引用了不存在的数据块";return false;}if(!db_.transaction()){error="无法开始事务";return false;}QSqlQuery q(db_);q.prepare("INSERT INTO snapshots(id,user_id,created_ms,name,manifest) VALUES(?,?,?,?,?)");q.addBindValue(id);q.addBindValue(user);q.addBindValue(QDateTime::currentMSecsSinceEpoch());q.addBindValue(o["name"].toString());q.addBindValue(QJsonDocument(o).toJson(QJsonDocument::Compact));if(!q.exec()){db_.rollback();error=q.lastError().text();return false;}for(auto v:chunks){QSqlQuery link(db_);link.prepare("INSERT INTO snapshot_chunks(snapshot_id,hash) VALUES(?,?)");link.addBindValue(id);link.addBindValue(v.toString());if(!link.exec()){db_.rollback();error=link.lastError().text();return false;}QSqlQuery ref(db_);ref.prepare("UPDATE chunks SET ref_count=ref_count+1 WHERE user_id=? AND hash=?");ref.addBindValue(user);ref.addBindValue(v.toString());ref.exec();}return db_.commit();}
    QByteArray snapshot(qint64 user,const QString&id,bool&ok){QSqlQuery q(db_);q.prepare("SELECT manifest FROM snapshots WHERE id=? AND user_id=?");q.addBindValue(id);q.addBindValue(user);ok=q.exec()&&q.next();return ok?q.value(0).toByteArray():QByteArray{};}
    bool deleteSnapshot(qint64 user,const QString&id,QString&error){if(!db_.transaction()){error="无法开始事务";return false;}QSqlQuery owner(db_);owner.prepare("SELECT 1 FROM snapshots WHERE id=? AND user_id=?");owner.addBindValue(id);owner.addBindValue(user);if(!owner.exec()||!owner.next()){db_.rollback();error="快照不存在";return false;}QStringList hashes;QSqlQuery list(db_);list.prepare("SELECT hash FROM snapshot_chunks WHERE snapshot_id=?");list.addBindValue(id);if(list.exec())while(list.next())hashes<<list.value(0).toString();QSqlQuery links(db_);links.prepare("DELETE FROM snapshot_chunks WHERE snapshot_id=?");links.addBindValue(id);links.exec();QSqlQuery snapshotDelete(db_);snapshotDelete.prepare("DELETE FROM snapshots WHERE id=? AND user_id=?");snapshotDelete.addBindValue(id);snapshotDelete.addBindValue(user);if(!snapshotDelete.exec()){db_.rollback();error=snapshotDelete.lastError().text();return false;}for(auto&hashValue:hashes){QSqlQuery ref(db_);ref.prepare("UPDATE chunks SET ref_count=ref_count-1 WHERE user_id=? AND hash=?");ref.addBindValue(user);ref.addBindValue(hashValue);ref.exec();}if(!db_.commit()){error="事务提交失败";return false;}for(auto&hashValue:hashes){QSqlQuery q(db_);q.prepare("SELECT ref_count FROM chunks WHERE user_id=? AND hash=?");q.addBindValue(user);q.addBindValue(hashValue);if(q.exec()&&q.next()&&q.value(0).toInt()<=0){QFile::remove(chunkPath(user,hashValue));QSqlQuery d(db_);d.prepare("DELETE FROM chunks WHERE user_id=? AND hash=? AND ref_count<=0");d.addBindValue(user);d.addBindValue(hashValue);d.exec();}}return true;}
 private: QString root_;QSqlDatabase db_;
};
}

int main(int argc,char**argv){QCoreApplication app(argc,argv);QCommandLineParser parser;parser.addHelpOption();parser.addOptions({{{"d","data"},"Data directory","path","server-data"},{{"c","cert"},"TLS certificate","path"},{{"k","key"},"TLS private key","path"},{{"p","port"},"Listen port","port","8443"},{{"l","listen"},"Listen address","address","127.0.0.1"}});parser.process(app);if(parser.value("cert").isEmpty()||parser.value("key").isEmpty())qFatal("TLS certificate and key are required");Store store;QString error;if(!store.open(parser.value("data"),error))qFatal("Database: %s",qPrintable(error));QHttpServer http;
    http.route("/api/v1/register",QHttpServerRequest::Method::Post,[&](const QHttpServerRequest&r){auto o=QJsonDocument::fromJson(r.body()).object();QString e;if(!store.registerUser(o["username"].toString(),o["password"].toString(),e))return json({{"error",e}},QHttpServerResponse::StatusCode::BadRequest);return json({{"ok",true}},QHttpServerResponse::StatusCode::Created);});
    http.route("/api/v1/login",QHttpServerRequest::Method::Post,[&](const QHttpServerRequest&r){auto o=QJsonDocument::fromJson(r.body()).object();QByteArray token=store.login(o["username"].toString(),o["password"].toString());return token.isEmpty()?json({{"error","登录失败"}},QHttpServerResponse::StatusCode::Unauthorized):json({{"token",QString::fromLatin1(token)},{"expiresIn",86400}});});
    http.route("/api/v1/logout",QHttpServerRequest::Method::Post,[&](const QHttpServerRequest&r){store.logout(r);return json({{"ok",true}});});
    http.route("/api/v1/status",QHttpServerRequest::Method::Get,[&](const QHttpServerRequest&r){qint64 u=store.authenticate(r);return u<0?json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized):json(store.status(u));});
    http.route("/api/v1/chunks/check",QHttpServerRequest::Method::Post,[&](const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized);QJsonArray missing;for(auto v:QJsonDocument::fromJson(r.body()).object()["hashes"].toArray())if(validHash(v.toString())&&!store.hasChunk(u,v.toString()))missing.append(v);return json({{"missing",missing}});});
    http.route("/api/v1/chunks/<arg>",QHttpServerRequest::Method::Put,[&](const QString&id,const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized);QString e;if(!validHash(id)||!store.putChunk(u,id,r.body(),e))return json({{"error",e}},QHttpServerResponse::StatusCode::BadRequest);return json({{"ok",true}});});
    http.route("/api/v1/chunks/<arg>",QHttpServerRequest::Method::Get,[&](const QString&id,const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return QHttpServerResponse(QHttpServerResponse::StatusCode::Unauthorized);bool ok=false;QByteArray data=store.getChunk(u,id,ok);return ok?QHttpServerResponse("application/octet-stream",data):QHttpServerResponse(QHttpServerResponse::StatusCode::NotFound);});
    http.route("/api/v1/snapshots",QHttpServerRequest::Method::Get,[&](const QHttpServerRequest&r){qint64 u=store.authenticate(r);return u<0?json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized):json({{"snapshots",store.snapshots(u)}});});
    http.route("/api/v1/snapshots",QHttpServerRequest::Method::Post,[&](const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized);QString e;if(!store.commitSnapshot(u,QJsonDocument::fromJson(r.body()).object(),e))return json({{"error",e}},QHttpServerResponse::StatusCode::BadRequest);return json({{"ok",true}},QHttpServerResponse::StatusCode::Created);});
    http.route("/api/v1/snapshots/<arg>",QHttpServerRequest::Method::Get,[&](const QString&id,const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized);bool ok=false;QByteArray data=store.snapshot(u,id,ok);return ok?QHttpServerResponse("application/json",data):json({{"error","快照不存在"}},QHttpServerResponse::StatusCode::NotFound);});
    http.route("/api/v1/snapshots/<arg>",QHttpServerRequest::Method::Delete,[&](const QString&id,const QHttpServerRequest&r){qint64 u=store.authenticate(r);if(u<0)return json({{"error","未授权"}},QHttpServerResponse::StatusCode::Unauthorized);QString e;return store.deleteSnapshot(u,id,e)?json({{"ok",true}}):json({{"error",e}},QHttpServerResponse::StatusCode::NotFound);});
    QFile certFile(parser.value("cert")),keyFile(parser.value("key"));
    if(!certFile.open(QIODevice::ReadOnly)||!keyFile.open(QIODevice::ReadOnly))qFatal("Cannot read TLS files");
    const QSslCertificate certificate(certFile.readAll(),QSsl::Pem);
    const QSslKey privateKey(keyFile.readAll(),QSsl::Rsa,QSsl::Pem);
    if(!QSslSocket::supportsSsl())qFatal("No usable Qt TLS backend is available");
    if(certificate.isNull()||privateKey.isNull())qFatal("Invalid TLS certificate or private key");
    QSslConfiguration tls=QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificate(certificate);tls.setPrivateKey(privateKey);tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSslServer server;server.setSslConfiguration(tls);
    QObject::connect(&server,&QSslServer::sslErrors,[](QSslSocket*,const QList<QSslError>&errors){for(const auto&e:errors)qWarning("TLS: %s",qPrintable(e.errorString()));});
    if(!server.listen(QHostAddress(parser.value("listen")),parser.value("port").toUShort())||!http.bind(&server))qFatal("Cannot listen");
    qInfo("DBackupServer listening on https://%s:%u",qPrintable(parser.value("listen")),server.serverPort());return app.exec();}
