cmake_minimum_required(VERSION 3.21)
project(DBackup VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "DBackup requires Linux")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
option(DBACKUP_BUILD_GUI "Build the Qt application and server" ON)
option(BUILD_TESTS "Build tests" ON)
option(DBACKUP_BUILD_TEST_CLI "Build the development test CLI" OFF)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)

add_library(dbackup_core STATIC
    archive/archive.cpp core/crypto.cpp core/linux_file.cpp repository/repository.cpp
    automation/cron.cpp automation/directory_watcher.cpp security/credential_store.cpp)
target_include_directories(dbackup_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dbackup_core PUBLIC ZLIB::ZLIB OpenSSL::Crypto Threads::Threads PkgConfig::SECRET)

if(DBACKUP_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Sql HttpServer WebSockets)
    qt_standard_project_setup()
    qt_add_executable(BackupTool
        main.cpp gui/main_window.cpp gui/main_window.h gui/filter_dialog.cpp gui/filter_dialog.h
        automation/job_manager.cpp automation/job_manager.h
        network/remote_client.cpp network/remote_client.h
        network/remote_repository_service.cpp network/remote_repository_service.h)
    target_link_libraries(BackupTool PRIVATE dbackup_core Qt6::Widgets Qt6::Network)
    qt_add_executable(DBackupServer server/server_main.cpp)
    target_link_libraries(DBackupServer PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::HttpServer)
    install(TARGETS BackupTool DBackupServer RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES docs/user-guide.md docs/server-guide.md docs/troubleshooting.md
    DESTINATION share/doc/dbackup)
