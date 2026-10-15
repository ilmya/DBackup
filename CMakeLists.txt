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
option(BUILD_TESTS "Build unit tests" ON)
option(DBACKUP_BUILD_TEST_CLI "Build the development-only test CLI" OFF)
option(DBACKUP_INCLUDE_SOFTWARE_OPENGL "Include the Qt software OpenGL fallback in portable packages" OFF)

if(MINGW)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE DBACKUP_MINGW_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(DBACKUP_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(DBACKUP_MINGW_ROOT "${DBACKUP_COMPILER_BIN}" DIRECTORY)
    list(APPEND CMAKE_PREFIX_PATH "${DBACKUP_MINGW_ROOT}/${DBACKUP_MINGW_TRIPLE}")
    foreach(DBACKUP_RUNTIME_DLL libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(NOT EXISTS "${DBACKUP_COMPILER_BIN}/${DBACKUP_RUNTIME_DLL}")
            message(FATAL_ERROR "The selected MinGW runtime is incomplete: ${DBACKUP_COMPILER_BIN}/${DBACKUP_RUNTIME_DLL}")
        endif()
        install(FILES "${DBACKUP_COMPILER_BIN}/${DBACKUP_RUNTIME_DLL}" DESTINATION .)
    endforeach()
endif()

find_package(ZLIB REQUIRED)

add_library(dbackup_core STATIC
    archive/archive.cpp archive/archive.h
    core/crypto.cpp core/crypto.h
    core/operation.h
    repository/repository.cpp repository/repository.h
    automation/cron.cpp automation/cron.h
    automation/directory_watcher.cpp automation/directory_watcher.h
    security/credential_store.cpp security/credential_store.h)
target_include_directories(dbackup_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dbackup_core PUBLIC bcrypt advapi32 ZLIB::ZLIB)
set_target_properties(dbackup_core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(DBACKUP_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Sql HttpServer)
    set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)
    qt_standard_project_setup()
    qt_add_executable(BackupTool WIN32
        main.cpp gui/main_window.cpp gui/main_window.h
        gui/filter_dialog.cpp gui/filter_dialog.h gui/theme.h
        automation/job_manager.cpp automation/job_manager.h
        network/remote_client.cpp network/remote_client.h
        network/remote_repository_service.cpp network/remote_repository_service.h)
    target_include_directories(BackupTool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BackupTool PRIVATE Qt6::Widgets Qt6::Network dbackup_core)
    set_target_properties(BackupTool PROPERTIES
        WIN32_EXECUTABLE TRUE OUTPUT_NAME "BackupTool"
        AUTOMOC_COMPILER_PREDEFINES OFF)
    install(TARGETS BackupTool RUNTIME DESTINATION .)
    qt_add_executable(DBackupServer server/server_main.cpp)
    set_target_properties(DBackupServer PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(DBackupServer PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::HttpServer)
    add_custom_command(TARGET DBackupServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE_DIR:DBackupServer>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Network> $<TARGET_FILE_DIR:DBackupServer>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Sql> $<TARGET_FILE_DIR:DBackupServer>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::HttpServer> $<TARGET_FILE_DIR:DBackupServer>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::WebSockets> $<TARGET_FILE_DIR:DBackupServer>)
    install(TARGETS DBackupServer RUNTIME DESTINATION .)
    # Keep executables and Qt plugins beside each other in the portable directory.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
    set(DBACKUP_DEPLOY_OPTIONS
        --no-translations
        --skip-plugin-types generic,iconengines,imageformats,networkinformation,styles
        --exclude-plugins qsqlmimer,qsqlodbc,qsqlpsql,qcertonlybackend,qschannelbackend)
    if(NOT DBACKUP_INCLUDE_SOFTWARE_OPENGL)
        list(APPEND DBACKUP_DEPLOY_OPTIONS --no-opengl-sw)
    endif()
    qt_generate_deploy_app_script(TARGET DBackupServer OUTPUT_SCRIPT server_deploy_script
        NO_TRANSLATIONS NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS ${DBACKUP_DEPLOY_OPTIONS})
    install(SCRIPT ${server_deploy_script})
    qt_generate_deploy_app_script(TARGET BackupTool OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS ${DBACKUP_DEPLOY_OPTIONS})
    install(SCRIPT ${deploy_script})

    # Qt's Schannel backend cannot serve a PEM private key directly. Package
    # the OpenSSL backend and OpenSSL 3 runtime from the explicitly selected root.
    if(DBACKUP_OPENSSL_ROOT)
        set(DBACKUP_OPENSSL_BIN "${DBACKUP_OPENSSL_ROOT}/bin")
    else()
        find_program(DBACKUP_OPENSSL_EXECUTABLE NAMES openssl)
        if(DBACKUP_OPENSSL_EXECUTABLE)
            get_filename_component(DBACKUP_OPENSSL_BIN "${DBACKUP_OPENSSL_EXECUTABLE}" DIRECTORY)
        endif()
    endif()
    if(DBACKUP_OPENSSL_BIN)
        file(TO_CMAKE_PATH "${DBACKUP_OPENSSL_BIN}" DBACKUP_OPENSSL_BIN)
    endif()
    if(DBACKUP_OPENSSL_BIN)
        set(DBACKUP_SSL_DLL "${DBACKUP_OPENSSL_BIN}/libssl-3-x64.dll")
        set(DBACKUP_CRYPTO_DLL "${DBACKUP_OPENSSL_BIN}/libcrypto-3-x64.dll")
        get_target_property(DBACKUP_QMAKE Qt6::qmake IMPORTED_LOCATION)
        execute_process(COMMAND "${DBACKUP_QMAKE}" -query QT_INSTALL_PLUGINS
            OUTPUT_VARIABLE DBACKUP_QT_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(DBACKUP_OPENSSL_PLUGIN "${DBACKUP_QT_PLUGIN_DIR}/tls/qopensslbackend.dll")
        if(EXISTS "${DBACKUP_SSL_DLL}" AND EXISTS "${DBACKUP_CRYPTO_DLL}" AND EXISTS "${DBACKUP_OPENSSL_PLUGIN}")
            install(FILES "${DBACKUP_SSL_DLL}" "${DBACKUP_CRYPTO_DLL}" DESTINATION .)
            install(FILES "${DBACKUP_OPENSSL_PLUGIN}" DESTINATION plugins/tls)
            set(DBACKUP_OPENSSL_DEPLOYED TRUE)
        else()
            set(DBACKUP_OPENSSL_DEPLOYED FALSE)
        endif()
    else()
        set(DBACKUP_OPENSSL_DEPLOYED FALSE)
    endif()
    install(CODE "if(NOT ${DBACKUP_OPENSSL_DEPLOYED})\n  message(FATAL_ERROR \"OpenSSL 3 runtime and Qt OpenSSL TLS plugin are required to create the portable server package\")\nendif()")
    if(NOT DBACKUP_INCLUDE_SOFTWARE_OPENGL)
        install(CODE "file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/D3Dcompiler_47.dll\")")
    endif()
endif()

if(BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTOUIC OFF)
    set(CMAKE_AUTORCC OFF)
    add_subdirectory(third_party/googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES docs/portable-readme.md DESTINATION . RENAME README.md)
install(FILES
    docs/user-guide.md docs/server-guide.md docs/troubleshooting.md docs/release-notes.md
    DESTINATION docs)
set(CPACK_PACKAGE_NAME "DBackup")
set(CPACK_PACKAGE_VENDOR "BackupTool Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
