/**
 * @file test_no_x_commands.cpp
 * @brief Tests for a build with FTP_SERVER_ENABLE_X_COMMANDS=0.
 *
 * The RFC 775 X-prefixed aliases cost one dispatch-table entry each and no
 * client sent since the 1990s needs them, so they can be compiled out. This
 * file pins both halves of that: the aliases are gone, and dropping them takes
 * nothing else with it.
 */
#include "ftp_test_support.hpp"

using namespace ftptest;

TEST_CASE("the X-prefixed aliases are absent when compiled out", "[commands]")
{
    init_server();
    Client c = connect_client();

    const char *aliases[] = {
        "XPWD\r\n", "XCWD sub\r\n", "XCUP\r\n", "XMKD d\r\n", "XRMD d\r\n",
    };
    for (const char *cmd : aliases) {
        INFO("command: " << cmd);
        REQUIRE(send_cmd(c, cmd) == "502 Command not implemented.\r\n");
    }
}

TEST_CASE("the commands the aliases stood for still work", "[commands]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
    REQUIRE(send_cmd(c, "CWD sub\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/sub\" is the current directory.\r\n");
    REQUIRE(send_cmd(c, "CDUP\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
    REQUIRE(send_cmd(c, "MKD newdir\r\n") == "257 \"/newdir\" created.\r\n");
    REQUIRE(send_cmd(c, "RMD olddir\r\n") == "250 Remove directory successful.\r\n");
}
