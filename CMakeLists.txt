cmake_minimum_required(VERSION 3.16)
project(BackupTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json 供 VSCode / cpplint 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ═══════════════════ 主程序 ═══════════════════
add_executable(BackupTool WIN32
    main.cpp
    packer.cpp
    packer.h
)

target_link_libraries(BackupTool PRIVATE comctl32)
set_target_properties(BackupTool PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(BackupTool PRIVATE -municode)

# ═══════════════════ gprof 性能分析 ═══════════════════
# 启用: cmake .. -DPROFILING=ON
# 运行后生成 gmon.out，用 gprof 分析
option(PROFILING "启用 gprof 性能分析" OFF)
if(PROFILING)
    message(STATUS ">>> gprof 性能分析已启用")
    target_compile_options(BackupTool PRIVATE -pg)
    target_link_options(BackupTool PRIVATE -pg)
endif()

# ═══════════════════ Google Test ═══════════════════
option(BUILD_TESTS "构建单元测试" ON)

if(BUILD_TESTS)
    # 使用本地 third_party/googletest（从 Gitee 镜像克隆）
    add_subdirectory(third_party/googletest)

    enable_testing()

    # 将 packer.cpp 编译为可复用对象库（主程序和测试共享）
    add_library(packer_lib STATIC packer.cpp packer.h)
    target_include_directories(packer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    # 测试可执行文件
    add_subdirectory(tests)
endif()
