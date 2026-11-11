add_executable(BackupToolTests
    # Core archive and crypto regression suite.
    test_archive.cpp
    test_crypto.cpp
    test_pipeline.cpp
    test_tar_rules.cpp
    test_linux_scan.cpp
    test_linux_filesystem.cpp
    test_repository.cpp
    test_repository_policy.cpp
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
set_tests_properties(CredentialUnavailableTest PROPERTIES LABELS integration)

if(TARGET Qt6::Core)
    add_executable(BackupToolAutomationTests
        test_job_manager.cpp
        test_job_policy.cpp
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

# GTest cases are discovered after build, so assign per-case labels from CTest.
# Keep this include last so both GTest test lists are available when CTest reads it.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CTestLabels.cmake")

if(TARGET Qt6::Widgets)
    add_executable(SourceDialogTest test_source_dialog.cpp ../gui/backup_options.cpp ../gui/source_tree.cpp ../gui/filter_dialog.cpp ../gui/filter_dialog.h)
    target_include_directories(SourceDialogTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(SourceDialogTest PRIVATE dbackup_core Qt6::Widgets GTest::gtest)
    add_test(NAME SourceDialogTest COMMAND ${CMAKE_COMMAND} -E env
        QT_QPA_PLATFORM=offscreen $<TARGET_FILE:SourceDialogTest>)
    set_tests_properties(SourceDialogTest PROPERTIES LABELS integration)
    add_executable(MainWindowLayoutTest test_main_window_layout.cpp
        ../gui/main_window.cpp ../gui/backup_options.cpp ../gui/source_tree.cpp ../gui/filter_dialog.cpp
        ../automation/job_manager.cpp)
    target_include_directories(MainWindowLayoutTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(MainWindowLayoutTest PRIVATE dbackup_core Qt6::Widgets GTest::gtest)
    add_test(NAME MainWindowLayoutTest COMMAND bash "${PROJECT_SOURCE_DIR}/tests/run_in_test_keyring.sh"
        "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen $<TARGET_FILE:MainWindowLayoutTest>)
    set_tests_properties(MainWindowLayoutTest PROPERTIES LABELS integration)
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
    set_tests_properties(BackupToolTestCliHelp PROPERTIES LABELS integration)

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
    set_tests_properties(AcceptanceToolSelfTest PROPERTIES LABELS support)
    add_test(NAME DemoComparisonTest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_demo_compare.py")
    set_tests_properties(DemoComparisonTest PROPERTIES LABELS support)
    add_test(NAME DemoVerifySelfTest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_demo_verify.py")
    set_tests_properties(DemoVerifySelfTest PROPERTIES LABELS support)
    add_test(NAME AcceptanceFixtureGenerate
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/prepare_acceptance_data.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --profile standard --reset)
    set_tests_properties(AcceptanceFixtureGenerate PROPERTIES LABELS support)
    add_test(NAME AcceptanceTestCliSmoke
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/acceptance_cli_smoke.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --test-cli $<TARGET_FILE:BackupToolTestCli>)
    set_tests_properties(AcceptanceTestCliSmoke PROPERTIES LABELS integration)
    add_test(NAME TarInterop
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tar_interop.py"
            --root "${DBACKUP_TEST_DATA_ROOT}" --cli $<TARGET_FILE:BackupToolTestCli>)
    set_tests_properties(TarInterop PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180 LABELS integration)
    set_tests_properties(AcceptanceTestCliSmoke PROPERTIES DEPENDS AcceptanceFixtureGenerate TIMEOUT 180)

    add_test(NAME AcceptanceScenarioCatalog
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/validate_acceptance_catalog.py")
    set_tests_properties(AcceptanceScenarioCatalog PROPERTIES LABELS support)
    set_tests_properties(AcceptanceToolSelfTest PROPERTIES
        ENVIRONMENT "DBACKUP_ACCEPTANCE_TEST_TMP=${DBACKUP_TEST_TEMP_ROOT}")
    set_tests_properties(BackupToolTestCliHelp AcceptanceTestCliSmoke PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${DBACKUP_TEST_PLUGIN_DIR}")
endif()
