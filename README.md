# DSync — 数据备份软件

> 软件开发课程实验项目

一款基于 C++ / Win32 API 的轻量级文件备份工具，支持自定义二进制格式（.abk）的打包与解包，保留文件元数据（权限、时间戳）。

## 功能特性

- **自定义打包格式**：`.abk` 二进制归档格式，小端序存储
- **完整元数据保留**：记录并恢复文件的 chmod 权限和 mtime 修改时间
- **递归目录处理**：支持嵌套子目录的完整打包与还原
- **极简图形界面**：Win32 原生 GUI，无需额外运行时依赖
- **单元测试覆盖**：10 个 Google Test 用例，覆盖核心打包/解包逻辑

## 环境要求

| 工具 | 版本要求 |
|------|---------|
| CMake | >= 3.16 |
| C++ 编译器 | 支持 C++17（MinGW g++ 8.1+ / MSVC 2019+） |
| 操作系统 | Windows 10/11 |

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
│   ── test_packer.cpp     # 单元测试（10 个用例）
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
│    Version  : uint16  (= 1)              │
│    Count    : uint32  (条目数)            │
│    Reserved : 8 bytes                    │
├──────────────────────────────────────────┤
│  Entry (每个文件/目录)                    │
│    Type     : uint8   (0=文件, 1=目录)   │
│    PathLen  : uint32                     │
│    Path     : UTF-8 字节                 │
│    Mode     : uint32  (chmod 权限)       │
│    Mtime    : int64   (ms 时间戳)        │
│    DataSize : uint64                     │
│    Data     : 文件内容                   │
└──────────────────────────────────────────┘
```

## 许可证

Copyright 2026 BackupTool Team. All rights reserved.
