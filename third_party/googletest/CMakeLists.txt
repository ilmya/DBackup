cmake_minimum_required(VERSION 3.21)

project(googletest-minimal LANGUAGES CXX)

add_library(gtest STATIC googletest/src/gtest-all.cc)
target_include_directories(gtest
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/googletest/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/googletest)
target_compile_features(gtest PUBLIC cxx_std_14)
add_library(GTest::gtest ALIAS gtest)

add_library(gtest_main STATIC googletest/src/gtest_main.cc)
target_link_libraries(gtest_main PUBLIC gtest)
add_library(GTest::gtest_main ALIAS gtest_main)
