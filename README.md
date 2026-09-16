# DSync — 数据备份软件

> 软件开发课程实验项目

一款基于 C++ / Win32 API 的轻量级文件备份工具，支持自定义二进制格式（.abk）的打包与解包，保留文件元数据（所有者、时间戳）。

## 功能特性

- **自定义打包格式**：`.abk` 二进制归档格式，小端序存储
- **完整元数据保留**：记录并恢复文件的 mtime 修改时间，并记录所有者信息
- **递归目录处理**：支持嵌套子目录的完整打包与还原
- **压缩备份**：使用 zlib 对每个文件压缩，只有压缩后更小才写入压缩数据
- **密码保护**：使用 Windows CNG 的 AES-256-CBC 加密和 SHA-256 完整性校验
- **自定义筛选**：按路径、名称、扩展名、类型、大小、修改日期和所有者筛选
- **极简图形界面**：Win32 原生 GUI，无需额外运行时依赖
- **单元测试覆盖**：12 个 Google Test 用例，覆盖核心打包/解包、加密和筛选逻辑

## 环境要求

| 工具 | 版本要求 |
|------|---------|
| CMake | >= 3.16 |
| C++ 编译器 | 支持 C++17（MinGW g++ 8.1+ / MSVC 2019+） |
| 操作系统 | Windows 10/11 |
| 运行库 | zlib；Windows 自带 CNG（bcrypt） |

## 快速开始

### 1. 克隆仓库

```bash
git clone https://github.com/伊尔米亚/DSync.git
cd DSync
```

### 2. 编译

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

编译完成后，`build/` 目录下生成 `BackupTool.exe`。

### 3. 运行

双击 `BackupTool.exe` 或在终端执行：

```bash
./build/BackupTool.exe
```

### 4. 使用

**打包备份：**
1. 切换到「️ 打包备份」标签页
2. 点击「选择目录」→ 选择要备份的文件夹
3. 点击「选择路径」→ 选择 `.abk` 文件保存位置
4. 点击「开始备份」

密码和筛选条件均为可选。筛选框使用分号分隔的键值表达式，例如：

```text
type=file;ext=.txt;path=docs;minsize=1024;maxsize=10485760;after=2025-01-01
```

支持的筛选键为 `path`、`name`、`ext`、`type`（`file`/`dir`）、`minsize`、`maxsize`、
`after`、`before` 和 `owner`。大小单位为字节，日期格式为 `YYYY-MM-DD`。

**还原解包：**
1. 切换到「📂 还原解包」标签页
2. 点击「选择文件」→ 选择 `.abk` 备份文件
3. 点击「选择目录」→ 选择还原目标文件夹
4. 点击「开始还原」

## 运行测试

```bash
cd build
cmake --build . --target BackupToolTests
./tests/BackupToolTests.exe
```

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
DSync/
── CMakeLists.txt          # CMake 构建配置
├── packer.h / packer.cpp   # 核心打包/解包引擎
├── main.cpp                # Win32 GUI 入口
├── tests/
│   ├── CMakeLists.txt
│   ── test_packer.cpp     # 单元测试（12 个用例）
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
│    Version  : uint16  (= 1 或 2)          │
│    Count    : uint32  (条目数)            │
│    Flags    : uint32  (bit 0 = 加密)      │
│    Reserved : uint32                      │
├──────────────────────────────────────────┤
│  Version 2 Entry (每个文件/目录)           │
│    Type/Flags/Path/Owner/Time              │
│    OriginalSize + StoredSize + Data        │
└──────────────────────────────────────────┘
```

版本 2 的加密归档在文件头后包含随机 salt 和 IV。版本 1 归档仍可读取，旧的无选项
`Packer::pack` 调用会继续生成版本 1 格式以保持兼容。

## 许可证

Copyright 2026 BackupTool Team. All rights reserved.
