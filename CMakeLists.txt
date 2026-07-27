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
#
# FTP_SERVER_USER / FTP_SERVER_PASS are compile-time constants, so the
# credential paths can only be tested by building a second copy of the
# library that defines them. Extra arguments become PUBLIC compile
# definitions, visible to the linked test file as well.
# --------------------------------------------------------------------
function(ftp_add_server_library name)
    add_library(${name} STATIC ${CMAKE_CURRENT_SOURCE_DIR}/ftp_server.c)
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PUBLIC ftp_mocks)
    if(ARGN)
        target_compile_definitions(${name} PUBLIC ${ARGN})
    endif()
    # -Wall/-Wextra first: without them -Werror guards almost nothing, since
    # the compiler's default warning set is nearly empty. The conversion and
    # shadow warnings are the ones that catch real defects in code this full of
    # uint16_t buffer arithmetic; ftp_server.c is clean under all of them.
    target_compile_options(${name} PRIVATE
        -Werror
        -Wall
        -Wextra
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wunused-function
        -Wunused-variable
        -Wunused-parameter
        -Wunused-but-set-variable
        -Wunused-macros
        -Wunused-const-variable
    )
    if(FTP_SERVER_ENABLE_CLANG_TIDY)
        set_target_properties(${name} PROPERTIES C_CLANG_TIDY "${FTP_SERVER_CLANG_TIDY_COMMAND}")
    endif()
    if(FTP_SERVER_ENABLE_COVERAGE)
        target_compile_options(${name} PRIVATE --coverage -O0 -g)
        target_link_options(${name} PUBLIC --coverage)
    endif()
endfunction()

ftp_add_server_library(ftp_server)

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

    include(CTest)
    include(Catch)

    function(ftp_add_test_executable name source library)
        add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${source})
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${name} PRIVATE ${library} Catch2::Catch2WithMain)
        catch_discover_tests(${name} TEST_PREFIX "${name}: ")
    endfunction()

    # Default build: no credentials configured, every client logged in on accept.
    ftp_add_test_executable(ftp_tests tests/test_main.cpp ftp_server)

    # Credential builds — see tests/test_auth.cpp.
    ftp_add_server_library(ftp_server_auth
        FTP_SERVER_USER=\"tester\" FTP_SERVER_PASS=\"s3cret\")
    ftp_add_test_executable(ftp_tests_auth tests/test_auth.cpp ftp_server_auth)

    ftp_add_server_library(ftp_server_user_only FTP_SERVER_USER=\"tester\")
    ftp_add_test_executable(ftp_tests_user_only tests/test_auth.cpp ftp_server_user_only)

    # Short path buffer — see tests/test_path_limits.cpp. Path overflow is
    # unreachable while FTP_SERVER_PATH_MAX equals FTP_SERVER_CMD_BUF_SIZE.
    ftp_add_server_library(ftp_server_short_path FTP_SERVER_PATH_MAX=64)
    ftp_add_test_executable(ftp_tests_short_path tests/test_path_limits.cpp
        ftp_server_short_path)

    # Short transfer buffer — see tests/test_list_truncation.cpp. A LIST line
    # cannot overflow the default 512-byte buffer with LFS_NAME_MAX at 255.
    ftp_add_server_library(ftp_server_short_data FTP_SERVER_DATA_BUF_SIZE=64)
    ftp_add_test_executable(ftp_tests_short_data tests/test_list_truncation.cpp
        ftp_server_short_data)

    # The obsolete RFC 775 aliases compiled out — see
    # tests/test_no_x_commands.cpp.
    ftp_add_server_library(ftp_server_no_x FTP_SERVER_ENABLE_X_COMMANDS=0)
    ftp_add_test_executable(ftp_tests_no_x tests/test_no_x_commands.cpp
        ftp_server_no_x)
endif()
