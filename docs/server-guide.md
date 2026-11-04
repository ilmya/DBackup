# DBackup Server 使用指南

先按 [README](../README.md) 构建 Linux 程序。以下命令使用专门的本机测试目录：

```bash
mkdir -p "$HOME/dbackup-server-test"
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 2 \
  -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -keyout "$HOME/dbackup-server-test/key.pem" -out "$HOME/dbackup-server-test/cert.pem"
./out/build-test/DBackupServer --data "$HOME/dbackup-server-test/data" \
  --cert "$HOME/dbackup-server-test/cert.pem" --key "$HOME/dbackup-server-test/key.pem" \
  --listen 127.0.0.1 --port 8443
```

该证书只供本机测试，私钥应保密。客户端连接 `https://127.0.0.1:8443` 时核对服务器证书 SHA-256 指纹。服务器面向本机或受信网络，不应直接暴露于公网。
