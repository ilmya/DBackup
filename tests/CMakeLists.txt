add_executable(BackupToolTests
    test_packer.cpp
)

target_link_libraries(BackupToolTests PRIVATE
    packer_lib
    GTest::gtest
    GTest::gtest_main
)

# gprof 支持（测试程序也可分析性能）
if(PROFILING)
    target_compile_options(BackupToolTests PRIVATE -pg)
    target_link_options(BackupToolTests PRIVATE -pg)
endif()

include(GoogleTest)
gtest_discover_tests(BackupToolTests)
