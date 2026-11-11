# 功能验证

本文件中较早的日期记录反映当时的代码和测试布局；当前自动化测试结构及最新执行结果以文末 2026-11-10 记录为准。历史执行记录不代表本轮 GUI 人工验收。

2026-11-05 在 WSL 2 Ubuntu 22.04、Qt 6.8.3 下运行：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/local-rework bash build.sh test
```

构建 GUI 和测试程序后，CTest 96 项通过。测试在 `~/DBackup-Demo` 的 Linux 原生文件系统中使用隔离的临时配置、D-Bus 会话和密钥环；不使用个人密钥环。

同日重新运行完整 CTest，96/96 通过；随后在同一隔离环境执行 `tests/demo.py run`，实际归档、恢复并比较 100 项，0 项失败。独立套接字往返与不可读文件报错通过；不同属主的提权案例、字符和块设备节点创建及恢复因缺少权限跳过。报告位于 `~/DBackup-Demo/reports/run-97246148.md` 和 `run-97246148-cases.md`。

覆盖范围包括归档与快照恢复、筛选和来源选择、压缩加密、错误密码、系统 tar 互操作、路径安全、套接字及其他特殊文件、普通权限不足、自动计划调度、实时监控、权限不足目录的部分监控、计划配置保存错误、独立归档不覆盖及保留策略。部分完成的归档和快照不会触发保留清理。

图形窗口的人工作业流程没有自动操作；无窗口 Qt 测试检查控件和布局，不等同于人工完成备份恢复。该历史轮次未进行设备节点实际创建和恢复的提权验证；原历史命令已移除，当前演示方法见用户指南。系统能力和文件系统仍可能不支持设备节点。

纯 tar 不提供内容认证；套接字使用 DBackup 的 PAX 扩展，普通 tar 工具可能将其提取为普通空文件。权限、链接和实时监控应优先在 WSL 原生文件系统验证，`/mnt/e` 等 Windows 文件系统挂载可能不具备相同语义。

2026-11-05 针对文件类型筛选、预览统计和权限提示重新运行隔离测试，CTest 98 项通过。演示目录新增 `test/dbackup_permission_case/no_read.txt`，普通用户备份时报告权限不足并继续处理 `readable.txt`；跳过不可读项后，恢复结果比较 111 项、0 项失败。报告：`~/DBackup-Demo/reports/run-f7699fe9.md`。图形窗口交互和提权设备节点仍未进行人工验证。

## 拆分前测试分层记录（历史基线）

2026-11-10 在 WSL 2 Ubuntu 22.04，GCC 11.4.0、CMake 3.22.1、Qt 6.8.3 下，使用项目内独立目录 `out/build-layered/` 执行：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-layered bash build.sh test-unit
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-layered bash build.sh test-other
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-layered bash build.sh test
DBACKUP_COVERAGE_BUILD_DIR=/mnt/e/Code/DSync/out/build-coverage-layered bash build.sh coverage
```

结果为 `test-unit` 17/17、`test-other` 81/81（77 integration、4 support）、完整 `test` 98/98；本轮没有跳过用例。`test-unit` 和 `test-other` 的 CTest 标签互斥，完整测试合并运行两组。所有集成测试都使用临时 D-Bus 会话与临时密钥环，未访问个人密钥环或 `/home/ilm/DBackup-Demo`。自动夹具位于构建目录。由于源码位于 `/mnt/e` 的 v9fs/DrvFS，构建脚本将需要 POSIX 语义的逐用例临时数据放在 `/tmp/dbackup-build-test-*`，并在命令退出时清理；测试输出数据仍限于自动夹具与临时案例。

本轮先让逐用例临时文件跟随 `/mnt/e` 构建目录，观察到 22 项因该挂载不支持权限位、纳秒时间戳、FIFO/socket、inotify 或相应原子文件操作而失败；将逐用例临时目录切换到 WSL 原生 `/tmp` 后，上述 `test-other` 与完整 `test` 全部通过。Vulkan headers 缺失仅产生可选 Qt 组件提示，不阻断构建。

覆盖率命令在独立 GCC Debug 构建中运行 17 个 unit 用例，生成 [行与分支摘要](../out/build-coverage-layered/coverage/summary.md) 和 gcov JSON 明细。报告为生产 core/automation 代码行覆盖 228/1712（13.3%）、分支覆盖 728/6961（10.5%）。该报告没有阈值，不覆盖 GUI、测试代码或第三方代码，也不是功能正确性的证明。

当时的单元测试还把四个临时配置/调度器驱动的 JobManager 用例列入 `unit`；本次已将它们重新归为集成测试，并由纯内存策略单测互补覆盖。WSLg 实际窗口显示、文件选择器、GUI 备份恢复、进度/日志可读性及关闭行为仍需人工验收；该记录没有启动或操作 GUI。

### 2026-11-10：纯逻辑单元测试拆分

在 Ubuntu 22.04 WSL、GCC 11.4、Qt 6.8.3 环境中，以新的隔离构建目录执行：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-policy bash build.sh test-unit
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-policy bash build.sh test-other
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-policy bash build.sh test
DBACKUP_COVERAGE_BUILD_DIR=/mnt/e/Code/DSync/out/build-coverage-policy bash build.sh coverage
```

最终结果：`test-unit` 29/29；`test-other` 85/85，其中 81 个 `integration`、4 个 `support`；完整 `test` 114/114。CTest 标签互斥且无漏标：29 个 `unit`、81 个 `integration`、4 个 `support`。测试没有跳过；隔离 D-Bus/密钥环流程由测试脚本提供，未读取个人密钥环，也未读取或清理 `/home/ilm/DBackup-Demo`。项目仍位于 `/mnt/e`，需要 POSIX 文件系统语义的测试临时数据由 `build.sh` 放在 WSL 原生 `/tmp`。

新增单元用例直接验证筛选条件与优先级、父级包含/子级排除/更深层重新包含、文件类型及归档元数据匹配、快照保留边界、计划输入校验和任务运行/排队决策。扫描器与文件树模型共用来源选择规则；任务管理器使用被测试的校验、队列合并和重跑决策；仓库清理使用被测试的保留计划。原先四个 `JobManager` 用例需要写临时计划配置或运行调度器，现归为集成测试并保留；真实扫描、归档、快照、权限、实时监控、配置持久化等集成回归没有删除或替代。

覆盖率使用独立目录并运行全部 29 个单元用例，结果为行覆盖 366/1858（19.7%）、分支覆盖 820/7135（11.5%）；[新摘要](../out/build-coverage-policy/coverage/summary.md) 和 gcov 明细保存在构建输出中。上一次拆分前的观察值为 228/1712 行（13.3%）、728/6961 分支（10.5%），当时运行 17 个单元用例。前后统计的代码版本和源文件集合不同；且本次将需配置文件/调度器的 JobManager 用例移回集成层，因此两组比例不能直接解释为同一代码上的覆盖率升降。该结果仅用于发现单元测试未触及的代码，不设门槛，也不证明功能正确性。

自动测试仍不模拟桌面点击。WSLg 实际显示、GUI 文件选择及备份恢复流程继续由用户手工验收；本次未启动或操作 GUI。

### 2026-11-10：源码整理与测试归组

在 WSL 2 Ubuntu 22.04、GCC 11.4.0、CMake 3.22.1、Qt 6.8.3 下，使用新建的 `out/build-refactor/` 和 `out/build-coverage-refactor/` 独立构建目录验证本轮修改。GUI、CLI 与测试程序均成功构建。运行命令：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-refactor bash build.sh test-unit
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-refactor bash build.sh test-other
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-refactor bash build.sh test
DBACKUP_COVERAGE_BUILD_DIR=/mnt/e/Code/DSync/out/build-coverage-refactor bash build.sh coverage
```

结果为 `test-unit` 19/19、`test-other` 84/84（80 个 `integration`、4 个 `support`）、完整测试 103/103。CTest 共 103 项：19 个 `unit`、80 个 `integration`、4 个 `support`；`test-unit` 与 `test-other` 标签互斥且无遗漏。本轮没有跳过用例。测试使用隔离的 D-Bus 会话和临时密钥环，不访问个人密钥环，也没有读取或清理 `/home/ilm/DBackup-Demo`。项目位于 `/mnt/e`，测试所需 POSIX 临时文件系统语义使用构建脚本提供的 WSL 原生 `/tmp` 临时目录。

独立 GCC 覆盖率构建运行 19 个单元测试并通过，生产代码行覆盖为 578/3464（16.7%），分支覆盖为 662/7075（9.4%）。[覆盖率摘要](../out/build-coverage-refactor/coverage/summary.md) 与 gcov 明细保存在该构建目录内；该数据没有阈值，只用于发现单元测试未触及的路径，不证明功能正确性。

本轮将过薄的仓库保留策略和任务策略实现并回仓库、任务管理器，保留扫描和来源选择共用的 `backup_policy`；合并重复测试套件和共享临时目录辅助逻辑。C++ 代码使用 `.clang-format` 统一格式。构建与自动测试验证不操作桌面窗口；WSLg 实际显示及 GUI 人工备份恢复仍未在本轮验证。


### 2026-11-10：计划、调度和仓库规则单元测试拆分

在 WSL 2 Ubuntu 22.04、GCC 11.4.0、CMake 3.22.1、Qt 6.8.3 下，使用独立构建目录运行：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-unit-refactor bash build.sh test-unit
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-unit-refactor bash build.sh test-other
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-unit-refactor bash build.sh test
DBACKUP_COVERAGE_BUILD_DIR=/mnt/e/Code/DSync/out/build-coverage-unit-refactor bash build.sh coverage
```

结果：单元测试 49/49；其他测试 83/83（79 integration、4 support）；完整测试 132/132，未跳过。CTest JSON 清单核对 49 个 unit、79 个 integration、4 个 support，每项恰有一个分类标签。集成测试使用临时 D-Bus 会话和密钥环，POSIX 临时数据位于 WSL 原生 `/tmp`。日志和清单保存在 `out/build-unit-refactor/`。

新增计划校验、任务运行与排队决策、快照保留数量、Cron 和 tar 格式边界的单元测试。仓库保留边界已合并到真实快照创建、清理与恢复测试，保留共享数据块、损坏清单和真实文件系统检查；任务管理器的执行器、持久化和实时监控集成测试仍保留。原有合并的 Cron 单测由更细的案例取代。

独立 GCC 覆盖率构建运行 49 个单元测试并通过：生产代码行覆盖 658/3476（18.9%），分支覆盖 772/7083（10.9%）。[摘要](../out/build-coverage-unit-refactor/coverage/summary.md) 与 gcov 明细位于独立构建目录。上一轮为 578/3464 行（16.7%）和 662/7075 分支（9.4%）；代码和统计分母不同，比例变化不能完全归因于测试数量。此报告只包含单元测试执行的生产代码，不包含 GUI、测试代码或第三方代码，不能证明功能正确。

本轮自动测试不操作真实桌面窗口；WSLg 显示、文件选择和 GUI 备份恢复流程仍需人工验收。


### 2026-11-10：计划显示和实时事件回归

在 WSL 2 Ubuntu 22.04、GCC 11.4.0、Qt 6.8.3 下，使用 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-user-review bash build.sh test` 运行完整自动测试：135/135 通过（49 unit、82 integration、4 support），无跳过。新增测试验证最近一次完整成功时间在部分完成、失败、取消和重新加载后保持，验证过大的封装归档可在读取内容前失败，以及实时监控创建、修改、重命名和删除事件。

完整归档依然按内存处理，DBackup 封装的 1 GiB 限制未取消；新预检仅对明显超限的普通文件大小合计提前报错。实时监控测试覆盖事件通知，不代表高负载性能、所有文件系统或 GUI 端到端验收通过。此轮未运行提权文件备份恢复，也未进行 GUI 桌面人工验收。


### 2026-11-10：Cron 兼容语义回归

在 WSL 2 Ubuntu 22.04 运行 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-user-review bash build.sh test`，139/139 通过（53 unit、82 integration、4 support），无跳过。新增验证受限日期和星期的或关系、星期日 7、跨多年闰日、夏令时跳过不存在的本地时间和不可匹配日期。计划保存时若未来八年找不到运行时间会返回错误。Cron 语义与原先日期和星期同时匹配不同，既有计划需检查。

未执行提权 CLI 演示、GUI 人工备份恢复、大归档流式改造及实时高负载端到端测试；用户指南中的提权命令是操作建议，不表示此轮已通过。

### 2026-11-10：切换系统 Google Test

在 WSL 2 Ubuntu 22.04 安装系统 `libgtest-dev` 1.11.0-3，CMake 找到 `/usr/lib/x86_64-linux-gnu/cmake/GTest` 中的 Google Test 1.11.0。移除项目内置 Google Test 后，使用独立目录运行：

```bash
DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-system-gtest bash build.sh test
cmake -S . -B out/build-no-gtest -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/home/ilm/Qt/6.8.3/gcc_64 -DBUILD_TESTS=OFF \
  -DDBACKUP_BUILD_TEST_CLI=OFF
cmake --build out/build-no-gtest --parallel 2
```

完整测试 139/139 通过（53 unit、82 integration、4 support），无跳过，与上一轮测试项数量一致；GUI 产品的无测试构建成功。使用临时 HOME、隔离 D-Bus 与临时密钥环，以 Qt offscreen 模式启动无测试构建的 GUI，程序运行至 3 秒超时结束，输出 offscreen 平台不提供托盘信号的提示；该检查不代表实际桌面窗口交互已验收。

### 2026-11-10：实时监控和构建入口整理

在 WSL 2 Ubuntu 22.04 使用 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-refine-fresh bash build.sh test`，完整测试 141/141 通过（53 unit、84 integration、4 support），无跳过；随后分别运行 `test-unit` 和 `test-other`，结果为 53/53 和 88/88。新增验证修改其他计划不丢失待处理的实时事件，以及暂停期间重建监控后恢复时请求重扫。修改一个计划时仅同步其监控，重建期间请求在监控重新就绪后发起完整重扫。

同一构建目录执行 `build` 后核验 `BUILD_TESTS` 与 `DBACKUP_BUILD_TEST_CLI` 均为 OFF；执行 `test-unit` 后二者均为 ON。归档扫描、格式、恢复和筛选解析拆分为独立源文件；演示工具保留比较入口，移除一次性生成和重置。未读取或清理手工演示目录。

本轮**尚未实现流式归档**：现有封装 1 GiB 限制和整体内存处理仍在，旧格式行为未改变；未进行大于 1 GiB 的流式往返测试。新增实时测试验证事件调度和重扫请求，不代表监控高负载或 GUI 端到端验收通过。

### 2026-11-10：封装大小上限调整

现有封装的打包数据与解压上限从 1 GiB 调整为 2 GiB；AES-GCM 更新调用分块，以避免单次调用的 `int` 长度限制。格式保持不变，归档仍整体按内存处理。使用 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-limit2 bash build.sh test`，143/143 项通过（55 unit、84 integration、4 support），无跳过。测试覆盖超过 2 GiB 的稀疏来源提前拒绝、2 GiB 解压边界判断和跨 AES 调用块的往返与认证失败；未执行超过 1 GiB 的真实数据归档恢复，因此该规模的内存需求和成功率尚未验证。

### 2026-11-10：合并测试入口与独立演示验收

`bash build.sh test` 合并运行全部自动测试；`test-unit` 和 `test-other` 已移除，以上各轮记录中的命令为历史命令。系统 Google Test 下完整回归 144/144 通过（55 unit、84 integration、5 support），无跳过。新增验收脚本的自测使用独立临时目录，不访问手工样例。

另以普通用户执行默认 `bash test.sh` 对已有三组样例进行备份恢复，输出目录为 `/home/ilm/DBackup-Demo/restore/dbackup-verify-ftfygjg4`：`filters` 为 PARTIAL（377 PASS、1 SKIP，约 0.37 秒）；`large-data` 在每进程 3072 MiB 限制下因 `std::bad_alloc` 备份失败（约 6.34 秒）；`special-files` 为 PARTIAL（83 PASS、3 SKIP，约 0.04 秒）。普通和提权执行使用相同的备份恢复流程：普通用户尝试恢复设备节点，因权限不足被程序跳过。另执行 tar、无压缩、加密的特殊文件验收，83 PASS、3 SKIP，报告位于 `restore/dbackup-verify-uwx5rbv2`。以上均为 CLI 核心验收，本轮未执行提权或 GUI 人工验收。
## 2026-11-11：分块归档与大文件验证

以下结果为本轮 WSL Ubuntu 普通用户执行；本文其他日期的结果均为历史记录，不代表当前实现。

- 使用独立构建目录执行 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-stream-wip bash build.sh test`，159 项通过、0 项失败、0 项跳过（57 unit、97 integration、5 support）。覆盖两种打包格式、四种压缩方式、加密开关、跨 4 MiB 边界、错误密码、损坏块、截断、块删除、重复和重排、源文件变化及写入失败。
- 在 WSL `/tmp` 独立临时目录实际写入 1,077,936,128 字节非稀疏文件，进程地址空间上限 1 GiB；自定义封装无压缩、自定义封装 DEFLATE 加密和增量仓库恢复的 SHA-256 均与来源一致。归档和恢复分别以独立 CLI 进程运行，样例使用重复数据；性能结果不代表随机数据的压缩速度。
- 普通用户以 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-stream-wip bash test.sh --memory-mib 1024` 验证三组现有样例：`filters` 部分完成（377 PASS、1 SKIP），`large-data` 通过（112 PASS），`special-files` 部分完成（83 PASS、3 SKIP）；无 FAIL 或 BLOCKED。跳过项为 root 属主、两个设备节点和不可读来源。报告保存在 `/home/ilm/DBackup-Demo/restore/dbackup-verify-adzzpnoh`。
- 尚未进行提权下的特殊文件完整恢复验收和 GUI 桌面交互验收。

同日将分块封装标识调整为 `DBPK` 加独立版本号，并验证读取此前生成的 `DBPK0002` 分块封装；重新运行完整自动测试，160 项通过、0 项失败、0 项跳过。

### 2026-11-11：现有演示数据压缩验收

普通用户在 WSL 执行 `DBACKUP_BUILD_DIR=/mnt/e/Code/DSync/out/build-stream-wip bash test.sh --memory-mib 1024`，默认自定义格式、DEFLATE、不加密；三组分别为 filters 377 PASS/1 SKIP、large-data 112 PASS、special-files 83 PASS/3 SKIP，均无 FAIL。跳过涉及原属主、设备节点和不可读来源，未进行提权及 GUI 手工验收。

在相同 1 GiB 数据上分别指定 `--compression none`、`--compression huffman`、`--compression lz77`、默认 DEFLATE、`--encrypt` 以及 `--format tar --compression none`，大数据逐项比较均通过。各自定义格式归档大小依次为 1,073,748,226、791,032,349、1,077,959,156、538,349,646 和 538,353,774 字节；测试报告和分项耗时保存在演示目录各次 `restore/dbackup-verify-*` 中。对 Huffman 和 LZ77 也单独验证了 filters、special-files；并未穷举所有打包、压缩和加密组合。脚本新增备份、恢复及总耗时和逻辑来源字节数、归档字节数输出，自测 9 项通过。

