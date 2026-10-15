# DBackup Server 使用指南

以下命令从仓库根目录的 Git Bash 执行。先构建并解压可运行的便携包：

```bash
cmd.exe /c build.bat package
mkdir -p out/package/acceptance
python -m zipfile -e out/package/DBackup-portable.zip out/package/acceptance
PORTABLE_DIR="$(find out/package/acceptance -mindepth 1 -maxdepth 1 -type d -print -quit)"
```

## 1. 准备证书

在 Git Bash 中运行：

```bash
./tests/acceptance.sh generate --root D:/DBackup-Acceptance --profile standard
./tests/acceptance.sh server-cert --output D:/DBackup-Acceptance/server-data/manual/cert
```

命令会生成有效期 30 天、仅供本机验收的证书和私钥。私钥不得提交到 Git。

## 2. 启动服务器

```bash
PORTABLE_DIR="$(find out/package/acceptance -mindepth 1 -maxdepth 1 -type d -print -quit)"
"$PORTABLE_DIR/DBackupServer.exe" \
  --data D:/DBackup-Acceptance/server-data/manual/data \
  --cert D:/DBackup-Acceptance/server-data/manual/cert/server-cert.pem \
  --key D:/DBackup-Acceptance/server-data/manual/cert/server-key.pem \
  --listen 127.0.0.1 \
  --port 8443
```

看到 `listening on https://127.0.0.1:8443` 后保持终端运行。

## 3. 自动检查

在第二个 Git Bash 窗口运行：

```bash
./tests/acceptance.sh server-smoke \
  --root D:/DBackup-Acceptance \
  --server https://127.0.0.1:8443
```

成功时命令输出 JSON 且退出码为 0。它会通过实际客户端执行注册、认证、快照上传与删除、分块隔离和恢复校验。测试账号会留在服务端数据库中，测试快照和令牌会清理；因此应使用专用、可丢弃的数据目录，不要连接生产服务器。

## 4. 客户端连接

1. 打开 DBackup 的“存储位置”。
2. 输入 `https://127.0.0.1:8443`。
3. 注册测试账户并登录。
4. 首次连接时核对证书 SHA-256 指纹后才确认。

## 5. 局域网连接

使用包含服务器 DNS 名或 IP SAN 的证书，将 `--listen` 改为局域网 IP。只为受信网络开放选定端口，并用两个不同账户确认数据不可互相查看。

## 6. 备份服务器数据

停止服务器后，同时备份数据目录内的 `server.sqlite` 和 `data` 目录。恢复时必须保持两者来自同一时点。

本服务不提供公网级防护、集群高可用或密码找回；需要公网访问时应放在成熟反向代理和访问控制之后。
