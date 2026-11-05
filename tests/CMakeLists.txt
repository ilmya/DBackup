add_executable(BackupToolTests
    # Core archive and crypto regression suite.
    test_archive.cpp
    test_crypto.cpp
    test_pipeline.cpp
    test_linux_file.cpp
    test_linux_scan.cpp
    test_linux_format.cpp
    test_linux_restore.cpp
    test_linux_archive.cpp
    test_special.cpp
    test_repository.cpp
    test_features.cpp
)

target_link_libraries(BackupToolTests PRIVATE
    dbackup_core
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(BackupToolTests)

# No session bus is reachable, so this test cannot touch a personal keyring.
add_executable(CredentialUnavailableTest test_credential_unavailable.cpp)
target_link_libraries(CredentialUnavailableTest PRIVATE dbackup_core GTest::gtest_main)
add_test(NAME CredentialUnavailableTest
    COMMAND ${CMAKE_COMMAND} -E env
        DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/dbackup-test-bus
        $<TARGET_FILE:CredentialUnavailableTest>)

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
        dbackup_core
        GTest::gtest)
    set_target_properties(BackupToolAutomationTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    gtest_discover_tests(BackupToolAutomationTests)
endif()

if(TARGET Qt6::Widgets)
    add_executable(SourceDialogTest test_source_dialog.cpp ../gui/backup_options.cpp ../gui/source_tree.cpp ../gui/filter_dialog.cpp ../gui/filter_dialog.h)
    target_include_directories(SourceDialogTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(SourceDialogTest PRIVATE dbackup_core Qt6::Widgets GTest::gtest)
    add_test(NAME SourceDialogTest COMMAND ${CMAKE_COMMAND} -E env
        QT_QPA_PLATFORM=offscreen $<TARGET_FILE:SourceDialogTest>)
    add_executable(MainWindowLayoutTest test_main_window_layout.cpp
        ../gui/main_window.cpp ../gui/backup_options.cpp ../gui/source_tree.cpp ../gui/filter_dialog.cpp
        ../automation/job_manager.cpp)
    target_include_directories(MainWindowLayoutTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(MainWindowLayoutTest PRIVATE dbackup_core Qt6::Widgets GTest::gtest)
    add_test(NAME MainWindowLayoutTest COMMAND bash "${PROJECT_SOURCE_DIR}/tests/run_in_test_keyring.sh"
        "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen $<TARGET_FILE:MainWindowLayoutTest>)
endif()

if(DBACKUP_BUILD_TEST_CLI)
    if(NOT TARGET Qt6::Core)
        message(FATAL_ERROR "The development test CLI requires Qt Core")
    endif()
    add_executable(BackupToolTestCli test_cli.cpp)
    target_include_directories(BackupToolTestCli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(BackupToolTestCli PRIVATE dbackup_core Qt6::Core)
    set_target_properties(BackupToolTestCli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    add_test(NAME BackupToolTestCliHelp COMMAND $<TARGET_FILE:BackupToolTestCli> --help)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(DBACKUP_TEST_DATA_ROOT "${CMAKE_BINARY_DIR}/acceptance-data" CACHE PATH "验收测试数据目录")
    set(DBACKUP_TEST_TEMP_ROOT "${CMAKE_BINARY_DIR}/acceptance-selftest-temp" CACHE PATH "验收工具自测临时目录")
    file(MAKE_DIRECTORY "${DBACKUP_TEST_TEMP_ROOT}")
    get_target_property(DBACKUP_TEST_QMAKE Qt6::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${DBACKUP_TEST_QMAKE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE DBACKUP_TEST_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(WRITE "${CMAKE_BINARY_DIR}/qt-plugin-dir.txt" "${DBACKUP_TEST_PLUGIN_DIR}")
    add_test(NAME AcceptanceToolSelfTest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/run_acceptance_selftest.py")
    add_test(NAME DemoComparisonTest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_demo_compare.py")
    add_test(NAME AcceptanceFixtureGenerate
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/prepare_acceptance_data.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --profile standard --reset)
    add_test(NAME AcceptanceTestCliSmoke
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/acceptance_cli_smoke.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --test-cli $<TARGET_FILE:BackupToolTestCli>)
    add_test(NAME TarInterop
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tar_interop.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --cli $<TARGET_FILE:BackupToolTestCli>)
    set_tests_properties(TarInterop PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180)
    set_tests_properties(AcceptanceTestCliSmoke PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180)

    add_test(NAME AcceptanceScenarioCatalog
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/validate_acceptance_catalog.py")
    set_tests_properties(AcceptanceToolSelfTest PROPERTIES
        ENVIRONMENT "DBACKUP_ACCEPTANCE_TEST_TMP=${DBACKUP_TEST_TEMP_ROOT}")
    set_tests_properties(BackupToolTestCliHelp AcceptanceTestCliSmoke PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${DBACKUP_TEST_PLUGIN_DIR}")
endif()
