cmake_minimum_required(VERSION 3.10)
project(ftp_lwip_lfs C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt-in clang-tidy pass over ftp_server.c/.h only (see .clang-tidy).
# Off by default so a missing clang-tidy binary never breaks a plain build.
option(FTP_SERVER_ENABLE_CLANG_TIDY "Run clang-tidy on ftp_server sources" OFF)
if(FTP_SERVER_ENABLE_CLANG_TIDY)
    find_program(FTP_SERVER_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(FTP_SERVER_CLANG_TIDY_COMMAND "${FTP_SERVER_CLANG_TIDY_EXE}")
endif()

# Opt-in coverage instrumentation for ftp_server.c, consumed by the CI
# `coverage` job to compute the README coverage badge. Off by default —
# instrumented builds are slower and irrelevant to normal development.
option(FTP_SERVER_ENABLE_COVERAGE "Build ftp_server with --coverage instrumentation" OFF)

# --------------------------------------------------------------------
# Mock library — stub lwIP + LFS implementations.
#
# The real lwip/lfs libraries are provided by the integrating project;
# this repo only uses these mocks to compile ftp_server.c for static
# analysis and unit testing.
# --------------------------------------------------------------------
add_library(ftp_mocks STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock/mock_lwip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock/mock_lfs.c
)
target_include_directories(ftp_mocks PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock
)

# --------------------------------------------------------------------
# ftp_server — compiled against mock headers
# --------------------------------------------------------------------
add_library(ftp_server STATIC ${CMAKE_CURRENT_SOURCE_DIR}/ftp_server.c)
target_include_directories(ftp_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ftp_server PUBLIC ftp_mocks)
target_compile_options(ftp_server PRIVATE
    -Werror
    -Wunused-function
    -Wunused-variable
    -Wunused-parameter
    -Wunused-but-set-variable
    -Wunused-macros
    -Wunused-const-variable
)
if(FTP_SERVER_ENABLE_CLANG_TIDY)
    set_target_properties(ftp_server PROPERTIES C_CLANG_TIDY "${FTP_SERVER_CLANG_TIDY_COMMAND}")
endif()
if(FTP_SERVER_ENABLE_COVERAGE)
    target_compile_options(ftp_server PRIVATE --coverage -O0 -g)
    target_link_options(ftp_server PUBLIC --coverage)
endif()

# --------------------------------------------------------------------
# Unit tests (opt-in via -DFTP_SERVER_BUILD_TESTS=ON)
# --------------------------------------------------------------------
option(FTP_SERVER_BUILD_TESTS "Build unit tests with Catch2 + mock lwIP/LFS" OFF)
if(FTP_SERVER_BUILD_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.8.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    endif()

    add_executable(ftp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
    )
    target_link_libraries(ftp_tests PRIVATE
        ftp_server
        Catch2::Catch2WithMain
    )

    include(CTest)
    include(Catch)
    catch_discover_tests(ftp_tests)
endif()
