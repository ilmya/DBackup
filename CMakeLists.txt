# 配置项目构建目标、依赖和测试
cmake_minimum_required(VERSION 3.21)
project(DBackup VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "DBackup requires Linux")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
option(DBACKUP_BUILD_GUI "Build the Qt application" ON)
option(BUILD_TESTS "Build tests" ON)
option(DBACKUP_BUILD_TEST_CLI "Build the development test CLI" OFF)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)

add_library(dbackup_core STATIC
    core/archive/archive.cpp core/archive/archive_scan.cpp core/archive/archive_format.cpp core/archive/archive_restore.cpp core/selection/filter_parser.cpp core/archive/archive_pipeline.cpp core/archive/archive_blocks.cpp core/archive/archive_custom_stream.cpp core/archive/archive_tar.cpp core/selection/backup_policy.cpp core/compression/compression.cpp core/security/md5.cpp core/security/crypto.cpp core/filesystem/linux_file.cpp
    automation/schedule/cron.cpp automation/realtime/directory_watcher.cpp core/security/credential_store.cpp core/incremental/incremental_diff.cpp core/incremental/incremental_scan.cpp core/incremental/incremental_manifest.cpp core/incremental/incremental_verify.cpp core/incremental/incremental_chain.cpp)
target_include_directories(dbackup_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dbackup_core PUBLIC ZLIB::ZLIB OpenSSL::Crypto Threads::Threads PkgConfig::SECRET)

if(DBACKUP_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
    qt_standard_project_setup()
    qt_add_executable(BackupTool
        main.cpp gui/main_window.cpp gui/main_window.h gui/backup_options.cpp gui/backup_options.h gui/source_tree.cpp gui/source_tree.h gui/filter_dialog.cpp gui/filter_dialog.h
        automation/job_manager.cpp automation/job_manager.h)
    target_link_libraries(BackupTool PRIVATE dbackup_core Qt6::Widgets)
    install(TARGETS BackupTool RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES docs/user-guide.md
    DESTINATION share/doc/dbackup)
