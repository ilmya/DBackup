# DBackup Server 使用指南

## 1. 准备证书

在 Git Bash 中运行：

```bash
./tools/acceptance/acceptance.sh server-cert --output D:/DBackup-Server/cert
```

命令会生成有效期 30 天、仅供本机验收的证书和私钥。私钥不得提交到 Git。

## 2. 启动服务器

```bash
./build/DBackupServer.exe \
  --data D:/DBackup-Server/data \
  --cert D:/DBackup-Server/cert/server-cert.pem \
  --key D:/DBackup-Server/cert/server-key.pem \
  --listen 127.0.0.1 \
  --port 8443
```

看到 `listening on https://127.0.0.1:8443` 后保持终端运行。

## 3. 自动检查

在第二个 Git Bash 窗口运行：

```bash
./tools/acceptance/acceptance.sh server-smoke \
  --server https://127.0.0.1:8443 \
  --report D:/DBackup-Acceptance/reports/server-smoke.md
```

报告中的注册、登录、状态和快照列表应全部为 `PASS`。

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
