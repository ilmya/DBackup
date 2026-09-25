# DBackup — 数据备份软件

> 软件开发课程实验项目

一款基于 C++17 / Qt 6 Widgets 的桌面备份工具，支持将多个文件和文件夹组合打包为自定义 `.abk` 归档并还原。

## 功能特性

- **自定义打包格式**：`.abk` 二进制归档格式，小端序存储
- **元数据支持**：记录并恢复 mtime 与 Windows 文件属性；记录所有者供筛选使用
- **灵活选择来源**：通过一个“添加来源”按钮添加多个文件或文件夹
- **递归目录处理**：支持嵌套子目录的完整打包与还原
- **压缩备份**：使用 zlib 对每个文件压缩，只有压缩后更小才写入压缩数据
- **密码保护**：新建 v3 归档默认使用 AES-256-GCM 认证加密，并保持旧 v1/v2（AES-256-CBC）读取兼容
- **可视化筛选面板**：提供类型预设、大小单位、时间快捷选项、常用排除和扫描预览
- **现代图形界面**：Qt 6 Widgets 响应式主窗口，包含概览、备份、恢复、计划、历史、存储和设置模块
- **可靠写入**：归档和恢复文件先写临时文件，成功后原子替换
- **自动与实时备份**：持久化 Cron 任务、版本淘汰、系统托盘和 `ReadDirectoryChangesW` 监控
- **块级增量仓库**：4 MiB 内容寻址块、SHA-256 去重、AES-256-GCM 与按版本恢复
- **网络备份原型**：TLS、注册登录、Token、SQLite、块级秒传/续传和用户隔离
- **单元测试覆盖**：28 个 Google Test 用例，覆盖 Sprint 1–5 可离线验证的核心行为

## 环境要求

| 工具 | 版本要求 |
|------|---------|
| CMake | >= 3.21 |
| Qt | >= 6.5，安装 Widgets、Network、Sql、HttpServer 及匹配的编译器套件 |
| C++ 编译器 | 支持 C++17，并与 Qt 套件 ABI 匹配（推荐 Qt 自带 MinGW 或 MSVC 2019+） |
| 操作系统 | Windows 10/11 |
| 网络 | 仅在本机没有 zlib 开发库时，用于自动下载固定版本 zlib |
| 服务器 TLS | OpenSSL 3（用于部署读取 PEM 私钥所需的 Qt OpenSSL 后端） |

Google Test 已包含在仓库中。CMake 会优先使用系统 zlib；找不到时默认自动下载并构建 zlib 1.3.1。Anaconda 附带的 Qt 5 不能用于本项目。

## 快速开始

### 1. 克隆仓库

```bash
git clone https://github.com/ilmya/DBackup.git
cd DBackup
```

### 2. 一键编译（推荐）

在 CMD 中执行：

```bat
build.bat
```

在 PowerShell 中执行：

```text
.\build.bat
```

也可以直接双击仓库根目录下的 `build.bat`。脚本会查找 `qmake6`、`QT_ROOT` 或标准的 `C:\Qt\6.x\mingw_64` 安装目录并执行增量构建。完成后生成：

```text
build\BackupTool.exe
```

常用操作：

```bat
build.bat test
build.bat package
build.bat core-test
```

| 命令 | 作用 |
|---|---|
| `build.bat` | 编译程序 |
| `build.bat test` | 编译并运行全部测试 |
| `build.bat package` | 编译并生成便携 ZIP |
| `build.bat core-test` | 未安装 Qt 时，仅编译核心并运行测试 |

### 3. 直接使用 CMake（可选）

下面的命令不依赖 PowerShell，在 CMD、PowerShell、Git Bash 等终端中均可使用：

```text
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64
cmake --build build -j 4
```

如果电脑已经安装 zlib 且需要完全离线构建，可禁止自动下载：

```text
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DDBACKUP_FETCH_ZLIB=OFF
```

如果系统没有 zlib，配置阶段会自动下载并随工程构建。Qt 安装位置不标准时，可以先执行 `set QT_ROOT=C:\Qt\6.8.3\mingw_64` 再运行 `build.bat`。

### 4. 运行

返回仓库根目录后双击 `build/BackupTool.exe`，或执行：

```text
build\BackupTool.exe
```

### 4. 使用

**打包备份：**
1. 在左侧导航切换到「新建备份」
2. 点击「添加来源」→ 选择“添加文件”或“添加文件夹”；可以多选或反复添加
3. 选择“便携完整归档”或“版本化增量仓库”，再选择对应文件或仓库目录
4. 点击「开始备份」

密码和筛选条件均为可选。点击“配置筛选...”可设置：

- 路径包含；
- 文件或文件夹类型；
- 名称和扩展名；
- 修改时间范围；
- 文件尺寸范围；
- 文件所有者。

界面会自动生成并校验筛选条件。六类条件按 AND 关系组合。底层使用的表达式示例如下：

为减少试错，类型可直接选择文档、图片、音频、视频、压缩包或程序代码预设；尺寸支持 B/KB/MB/GB；时间支持今天、最近 7 天和最近 30 天；还可一键排除临时文件、系统缓存、编译输出和日志。点击“扫描预览”只读取文件元数据，不生成归档，即可查看预计包含/排除数量和总大小。

```text
type=file;ext=.txt;path=docs;minsize=1024;maxsize=10485760;after=2025-01-01
```

支持的筛选键为 `path`、`name`、`ext`、`type`（`file`/`dir`）、`minsize`、`maxsize`、
`after`、`before` 和 `owner`。大小单位为字节，日期格式为 `YYYY-MM-DD`。
未知键、非法数字、非法日期或矛盾范围会在备份开始前显示错误。

**还原解包：**
1. 在左侧导航切换到「恢复数据」
2. 点击「选择文件」→ 选择 `.abk` 备份文件
3. 点击「选择目录」→ 选择还原目标文件夹
4. 点击「开始还原」

### 启动课程服务器

服务器强制使用 TLS。准备 PEM 格式证书和 RSA 私钥后运行：

```text
build\DBackupServer.exe --data server-data --cert server.crt --key server.key
```

默认只监听 `127.0.0.1:8443`。发布服务器前请确保 `openssl.exe` 可从 `PATH` 找到；`build.bat` 会让 `windeployqt` 同时部署 Qt OpenSSL 后端和 OpenSSL 3 DLL。该服务是课程及受信网络原型，不应直接暴露到公网。

## 运行测试

```text
build.bat test
```

## 生成便携发布包

Release 构建完成后执行：

```text
build.bat package
```

输出文件为 `build/DBackup-1.0.0-win64.zip`，压缩包内的程序仍名为 `BackupTool.exe`。CMake 会通过 Qt 部署脚本把所需 Qt 运行库和平台插件一并放入安装包，目标电脑不需要预装 Qt。

也可以生成未压缩的安装目录：

```text
cmake --install build --prefix install
```

`build/`、`tmp/`、`install/`、`package/`、下载依赖目录和 ZIP 产物均已加入 `.gitignore`。

## 项目文档

- [需求分析说明书](docs/requirements.md)
- [系统设计文档](docs/design.md)
- [软件测试报告](docs/test-report.md)
- [Sprint 1–5 完成度与待验证事项](docs/progress.md)

v3 支持记录符号链接、目录联接和 Windows 安全描述符，并默认拒绝可能逃逸恢复目录的危险链接。创建链接和恢复 ACL 仍受当前 Windows 账户权限约束。

## 代码质量检测

```bash
cpplint --linelength=120 --filter=-build/c++11,-runtime/references,-whitespace/braces,-readability/casting,-build/include_order,-build/include_subdir,-runtime/string *.cpp *.h tests/*.cpp
```

## 性能分析（可选）

```bash
cd build
cmake .. -DPROFILING=ON
cmake --build .
./BackupTool.exe          # 运行后生成 gmon.out
gprof BackupTool.exe gmon.out > analysis.txt
```

## 项目结构

```
DBackup/
── CMakeLists.txt          # CMake 构建配置
├── packer.h / packer.cpp   # 核心打包/解包引擎
├── main.cpp                # Qt 6 应用入口
├── gui/                    # 主窗口、筛选对话框和统一主题
├── core/                   # 操作上下文与公共类型
├── repository/             # 本地内容寻址增量仓库
├── automation/             # Cron、任务管理与目录监控
├── network/                # 远程服务器客户端
├── security/               # Windows Credential Manager 封装
├── server/                 # DBackupServer HTTP/TLS/SQLite 服务端
├── build.bat               # CMD/PowerShell 通用的一键构建脚本
├── tests/
│   ├── CMakeLists.txt
│   ├── test_packer.cpp
│   └── test_sprint345.cpp # Sprint 3–5 自动化测试（共注册 28 项）
├── docs/                   # 需求、设计和测试文档
├── third_party/
│   └── googletest/         # Google Test v1.14.0
── .vscode/                # VSCode 开发配置
└── .gitignore
```

## .abk 文件格式

```
──────────────────────────────────────────┐
│  Header (20 bytes)                       │
│    Magic    : "ABKPKG"  (6 bytes)        │
│    Version  : uint16  (= 1、2 或 3)        │
│    Count    : uint32  (条目数)            │
│    Flags    : uint32  (bit 0 = 加密)      │
│    Reserved : uint32                      │
├──────────────────────────────────────────┤
│  Version 3 Entry (文件/目录/重解析点)       │
│    Type/Flags/Path/Owner/Time/ACL/Link     │
│    OriginalSize + StoredSize + Data        │
└──────────────────────────────────────────┘
```

新 v3 加密归档使用随机 Salt、Nonce 和 AES-256-GCM 标签。版本 1/2 归档仍可读取；旧的无选项 `Packer::pack` 调用会继续生成版本 1 格式以保持源码和格式兼容。

## 许可证

Copyright 2026 BackupTool Team. All rights reserved.
