# DBackup

DBackup backs up and restores files on Windows 10/11.

## Start

- Run `BackupTool.exe` to create or restore a local backup.
- Run `DBackupServer.exe --help` to see server options.
- Keep the `docs` folder beside the programs for the user and server guides.

The portable folder can be copied to another Windows 10/11 x64 computer. It does not require Qt, CMake, MinGW, or Python to be installed.

## Server

The server requires a TLS certificate, private key, and a writable data directory. Use it on the local computer or a trusted network. Do not expose it directly to the public internet.

See `docs/user-guide.md` and `docs/server-guide.md` for operation instructions.
