cmake_minimum_required(VERSION 3.21)
project(DBackup VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DBackup currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)

option(DBACKUP_BUILD_GUI "Build the Qt 6 Widgets application" ON)
option(DBACKUP_FETCH_ZLIB "Download zlib when it is not installed" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(PROFILING "Enable gprof profiling" OFF)

if(MINGW)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE DBACKUP_MINGW_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE)
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
    FetchContent_Declare(zlib URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz)
    FetchContent_MakeAvailable(zlib)
    set(DBACKUP_ZLIB_TARGET zlibstatic)
else()
    message(FATAL_ERROR "zlib was not found. Install zlib or enable DBACKUP_FETCH_ZLIB")
endif()

add_library(dbackup_core STATIC
    packer.cpp packer.h
    core/operation.h
    repository/repository.cpp repository/repository.h
    automation/cron.cpp automation/cron.h
    automation/directory_watcher.cpp automation/directory_watcher.h
    security/credential_store.cpp security/credential_store.h)
target_include_directories(dbackup_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dbackup_core PUBLIC bcrypt advapi32 ${DBACKUP_ZLIB_TARGET})

if(DBACKUP_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Sql HttpServer)
    qt_standard_project_setup()
    qt_add_executable(BackupTool WIN32
        main.cpp gui/main_window.cpp gui/main_window.h
        gui/filter_dialog.cpp gui/filter_dialog.h gui/theme.h
        automation/job_manager.cpp automation/job_manager.h
        network/remote_client.cpp network/remote_client.h
        network/remote_repository_service.cpp network/remote_repository_service.h)
    target_include_directories(BackupTool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BackupTool PRIVATE Qt6::Widgets Qt6::Network dbackup_core)
    set_target_properties(BackupTool PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "BackupTool")
    if(PROFILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(BackupTool PRIVATE -pg)
        target_link_options(BackupTool PRIVATE -pg)
    endif()
    install(TARGETS BackupTool RUNTIME DESTINATION .)
    qt_add_executable(DBackupServer server/server_main.cpp)
    target_link_libraries(DBackupServer PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::HttpServer)
    install(TARGETS DBackupServer RUNTIME DESTINATION .)
    # Keep Windows executables and their runtime DLLs together.  The default
    # GNU-style "bin" deployment directory is not on the Windows DLL search path.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
    qt_generate_deploy_app_script(TARGET DBackupServer OUTPUT_SCRIPT server_deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${server_deploy_script})
    qt_generate_deploy_app_script(TARGET BackupTool OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})

    # Qt's Schannel backend cannot serve a PEM private key directly.  Package
    # the OpenSSL backend and OpenSSL 3 runtime when they are available.
    find_program(DBACKUP_OPENSSL_EXECUTABLE NAMES openssl)
    if(DBACKUP_OPENSSL_EXECUTABLE)
        get_filename_component(DBACKUP_OPENSSL_BIN "${DBACKUP_OPENSSL_EXECUTABLE}" DIRECTORY)
        find_file(DBACKUP_SSL_DLL NAMES libssl-3-x64.dll PATHS "${DBACKUP_OPENSSL_BIN}" NO_DEFAULT_PATH)
        find_file(DBACKUP_CRYPTO_DLL NAMES libcrypto-3-x64.dll PATHS "${DBACKUP_OPENSSL_BIN}" NO_DEFAULT_PATH)
        get_target_property(DBACKUP_QMAKE Qt6::qmake IMPORTED_LOCATION)
        execute_process(COMMAND "${DBACKUP_QMAKE}" -query QT_INSTALL_PLUGINS
            OUTPUT_VARIABLE DBACKUP_QT_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(DBACKUP_OPENSSL_PLUGIN "${DBACKUP_QT_PLUGIN_DIR}/tls/qopensslbackend.dll")
        if(DBACKUP_SSL_DLL AND DBACKUP_CRYPTO_DLL AND EXISTS "${DBACKUP_OPENSSL_PLUGIN}")
            install(FILES "${DBACKUP_SSL_DLL}" "${DBACKUP_CRYPTO_DLL}" DESTINATION .)
            install(FILES "${DBACKUP_OPENSSL_PLUGIN}" DESTINATION plugins/tls)
        else()
            message(WARNING "OpenSSL 3 runtime was not found; the portable DBackupServer package will not support PEM TLS keys")
        endif()
    endif()
endif()

if(BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)
    enable_testing()
    add_library(packer_lib ALIAS dbackup_core)
    add_subdirectory(tests)
endif()

install(FILES README.md DESTINATION .)
install(FILES
    docs/user-guide.md docs/server-guide.md docs/testing-guide.md
    docs/acceptance-test-cases.md docs/troubleshooting.md docs/release-notes.md
    DESTINATION docs)
set(CPACK_PACKAGE_NAME "DBackup")
set(CPACK_PACKAGE_VENDOR "BackupTool Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
