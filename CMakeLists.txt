cmake_minimum_required(VERSION 3.16)
project(DBackup VERSION 1.0.0 LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "DBackup currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json 供 VSCode / cpplint 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prefer an installed zlib.  If it is missing, download and build a pinned copy
# so a new Windows development machine only needs CMake, a compiler and Git/network.
option(DBACKUP_FETCH_ZLIB "Download zlib automatically when it is not installed" ON)
if(MINGW)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE DBACKUP_MINGW_TRIPLE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    get_filename_component(DBACKUP_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(DBACKUP_MINGW_ROOT "${DBACKUP_COMPILER_BIN}" DIRECTORY)
    list(APPEND CMAKE_PREFIX_PATH "${DBACKUP_MINGW_ROOT}/${DBACKUP_MINGW_TRIPLE}")
endif()
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    set(DBACKUP_ZLIB_TARGET ZLIB::ZLIB)
elseif(DBACKUP_FETCH_ZLIB)
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib
        URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    )
    FetchContent_MakeAvailable(zlib)
    set(DBACKUP_ZLIB_TARGET zlibstatic)
else()
    message(FATAL_ERROR "zlib was not found. Install zlib or configure with -DDBACKUP_FETCH_ZLIB=ON")
endif()

function(dbackup_use_static_mingw_runtime target_name)
    if(MINGW)
        target_link_options(${target_name} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endfunction()

# ═══════════════════ 主程序 ═══════════════════
add_executable(BackupTool WIN32
    main.cpp
    packer.cpp
    packer.h
)

target_link_libraries(BackupTool PRIVATE
    comctl32 comdlg32 ole32 gdi32 bcrypt advapi32 ${DBACKUP_ZLIB_TARGET}
)
set_target_properties(BackupTool PROPERTIES WIN32_EXECUTABLE TRUE)
if(MINGW)
    target_link_options(BackupTool PRIVATE -municode)
endif()
dbackup_use_static_mingw_runtime(BackupTool)

# ═══════════════════ gprof 性能分析 ═══════════════════
# 启用: cmake .. -DPROFILING=ON
# 运行后生成 gmon.out，用 gprof 分析
option(PROFILING "启用 gprof 性能分析" OFF)
if(PROFILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS ">>> gprof 性能分析已启用")
    target_compile_options(BackupTool PRIVATE -pg)
    target_link_options(BackupTool PRIVATE -pg)
endif()

# ═══════════════════ Google Test ═══════════════════
option(BUILD_TESTS "构建单元测试" ON)

if(BUILD_TESTS)
    # 使用本地 third_party/googletest（从 Gitee 镜像克隆）
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)

    enable_testing()

    # 将 packer.cpp 编译为可复用对象库（主程序和测试共享）
    add_library(packer_lib STATIC packer.cpp packer.h)
    target_include_directories(packer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(packer_lib PUBLIC bcrypt advapi32 ${DBACKUP_ZLIB_TARGET})

    # 测试可执行文件
    add_subdirectory(tests)
endif()

# `cmake --install` creates a portable application directory.  CPack can turn
# it into a ZIP suitable for copying to a Windows machine without a compiler.
install(TARGETS BackupTool RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(DIRECTORY docs DESTINATION .)

set(CPACK_PACKAGE_NAME "DBackup")
set(CPACK_PACKAGE_VENDOR "BackupTool Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
