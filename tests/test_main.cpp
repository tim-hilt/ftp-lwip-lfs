/**
 * @file test_main.cpp
 * @brief Entry point for Catch2 FTP server unit tests.
 *
 * Uses Catch2WithMain — no custom main() needed.
 * Add more TEST_CASE blocks here or in additional .cpp files linked to the
 * ftp_tests target.
 */
#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "ftp_server.h"
#include "mock_lwip.h"
#include "mock_lfs.h"
}

TEST_CASE("Mock scaffold compiles and links", "[smoke]") {
    mock_lwip_reset();
    mock_lfs_reset();

    /* ftp_server_init(NULL) should return ERR_ARG. */
    REQUIRE(ftp_server_init(NULL) == ERR_ARG);
}
