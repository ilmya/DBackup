add_executable(BackupToolTests
    # Core archive and crypto regression suite.
    test_archive.cpp
    test_crypto.cpp
    test_sprint345.cpp
)

target_link_libraries(BackupToolTests PRIVATE
    dbackup_core
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(BackupToolTests)

if(TARGET Qt6::Core)
    add_executable(BackupToolAutomationTests
        test_job_manager.cpp
        test_job_manager_main.cpp
        ../automation/job_manager.cpp
        ../automation/cron.cpp
        ../automation/directory_watcher.cpp
        ../automation/job_manager.h)
    target_include_directories(BackupToolAutomationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(BackupToolAutomationTests PRIVATE
        Qt6::Core
        GTest::gtest)
    set_target_properties(BackupToolAutomationTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    gtest_discover_tests(BackupToolAutomationTests)
endif()

if(DBACKUP_BUILD_TEST_CLI)
    if(NOT TARGET DBackupServer OR NOT TARGET Qt6::Network)
        message(FATAL_ERROR "The development test CLI requires the Qt client and DBackupServer targets")
    endif()
    add_executable(BackupToolTestCli
        test_cli.cpp
        ../network/remote_client.cpp ../network/remote_client.h
        ../network/remote_repository_service.cpp ../network/remote_repository_service.h)
    target_include_directories(BackupToolTestCli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(BackupToolTestCli PRIVATE dbackup_core Qt6::Core Qt6::Network)
    add_custom_command(TARGET BackupToolTestCli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE_DIR:BackupToolTestCli>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Network> $<TARGET_FILE_DIR:BackupToolTestCli>)
    set_target_properties(BackupToolTestCli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    add_test(NAME BackupToolTestCliHelp COMMAND $<TARGET_FILE:BackupToolTestCli> --help)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(DBACKUP_TEST_OPENSSL NAMES openssl openssl.exe REQUIRED)
    get_filename_component(DBACKUP_TEST_OPENSSL_BIN "${DBACKUP_TEST_OPENSSL}" DIRECTORY)
    foreach(DBACKUP_TEST_SSL_DLL libssl-3-x64.dll libcrypto-3-x64.dll)
        if(EXISTS "${DBACKUP_TEST_OPENSSL_BIN}/${DBACKUP_TEST_SSL_DLL}")
            configure_file("${DBACKUP_TEST_OPENSSL_BIN}/${DBACKUP_TEST_SSL_DLL}"
                "${CMAKE_BINARY_DIR}/${DBACKUP_TEST_SSL_DLL}" COPYONLY)
        endif()
    endforeach()
    set(DBACKUP_TEST_DATA_ROOT "${CMAKE_BINARY_DIR}/acceptance-e2e")
    set(DBACKUP_TEST_TEMP_ROOT "${CMAKE_BINARY_DIR}/acceptance-e2e/selftest-temp")
    file(MAKE_DIRECTORY "${DBACKUP_TEST_TEMP_ROOT}")
    get_target_property(DBACKUP_TEST_QMAKE Qt6::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${DBACKUP_TEST_QMAKE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE DBACKUP_TEST_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(WRITE "${CMAKE_BINARY_DIR}/qt-plugin-dir.txt" "${DBACKUP_TEST_PLUGIN_DIR}")
    add_test(NAME AcceptanceToolSelfTest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/run_acceptance_selftest.py")
    add_test(NAME AcceptanceFixtureGenerate
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/prepare_acceptance_data.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --profile standard --reset)
    add_test(NAME AcceptanceTestCliSmoke
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/acceptance_cli_smoke.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --test-cli $<TARGET_FILE:BackupToolTestCli>)
    set_tests_properties(AcceptanceTestCliSmoke PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180)
    add_test(NAME DBackupRemoteClientServerE2E
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/acceptance_network_e2e.py"
            --root "${DBACKUP_TEST_DATA_ROOT}"
            --server-exe $<TARGET_FILE:DBackupServer>
            --test-cli $<TARGET_FILE:BackupToolTestCli>
            --openssl "${DBACKUP_TEST_OPENSSL}")
    set_tests_properties(DBackupRemoteClientServerE2E PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180)

    add_test(NAME AcceptanceScenarioCatalog
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/validate_acceptance_catalog.py")
    set_tests_properties(AcceptanceToolSelfTest PROPERTIES
        ENVIRONMENT "DBACKUP_ACCEPTANCE_TEST_TMP=${DBACKUP_TEST_TEMP_ROOT}")
    set_tests_properties(BackupToolTestCliHelp AcceptanceTestCliSmoke DBackupRemoteClientServerE2E PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${DBACKUP_TEST_PLUGIN_DIR}")
endif()
