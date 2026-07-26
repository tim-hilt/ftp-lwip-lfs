/**
 * @file test_path_limits.cpp
 * @brief Path-length handling when FTP_SERVER_PATH_MAX is smaller than
 *        FTP_SERVER_CMD_BUF_SIZE.
 *
 * At the default configuration both are 256, so no command line can even carry
 * an absolute path that overflows the path buffer — the "500 Command line too
 * long" guard fires first. Embedded builds routinely shrink FTP_SERVER_PATH_MAX
 * on its own, which is what makes overflow reachable, so these cases run
 * against a library built with a deliberately small path buffer.
 */
#include "ftp_test_support.hpp"

using namespace ftptest;

static_assert(FTP_SERVER_PATH_MAX < FTP_SERVER_CMD_BUF_SIZE - 16,
              "this file must be linked against a short-FTP_SERVER_PATH_MAX build");

TEST_CASE("an absolute path truncated at a separator must not resolve to its parent",
          "[path][security]")
{
    /* The dangerous shape: cutting the path at exactly FTP_SERVER_PATH_MAX - 1
     * bytes lands on a '/', so the leftover normalises cleanly into the
     * *parent directory* of the requested file. Silently accepting that turns
     * "DELE <deep file>" into "remove the directory holding it". */
    init_server();
    Client c = connect_client();

    static std::string s_removed;
    s_removed.clear();
    mock_lfs_stat_fn   = stat_is_reg;
    mock_lfs_remove_fn = [](lfs_t *, const char *path) -> int {
        s_removed = path;
        return 0;
    };

    /* "/<dir>/target.bin", sized so the truncation point is the separator. */
    const std::string dir(FTP_SERVER_PATH_MAX - 3, 'd');
    const std::string arg = "/" + dir + "/target.bin";
    REQUIRE(arg.size() > FTP_SERVER_PATH_MAX - 1);

    REQUIRE(send_cmd(c, "DELE " + arg + "\r\n") == "550 Path too long.\r\n");
    REQUIRE(s_removed.empty());
}

TEST_CASE("an over-long path component is rejected", "[path]")
{
    init_server();
    Client c = connect_client();

    static std::string s_removed;
    s_removed.clear();
    mock_lfs_stat_fn   = stat_is_reg;
    mock_lfs_remove_fn = [](lfs_t *, const char *path) -> int {
        s_removed = path;
        return 0;
    };

    const std::string too_long(FTP_SERVER_PATH_MAX + 8, 'a');

    SECTION("DELE") {
        REQUIRE(send_cmd(c, "DELE /" + too_long + "\r\n") == "550 Path too long.\r\n");
        REQUIRE(s_removed.empty());
    }
    SECTION("RETR") {
        REQUIRE(send_cmd(c, "RETR /" + too_long + "\r\n") == "550 Path too long.\r\n");
    }
    SECTION("CWD") {
        REQUIRE(send_cmd(c, "CWD /" + too_long + "\r\n") == "550 Path too long.\r\n");
        /* The rejected path must not have become the working directory. */
        REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
    }
}

TEST_CASE("an absolute path that fits is still accepted", "[path]")
{
    init_server();
    Client c = connect_client();

    static std::string s_removed;
    s_removed.clear();
    mock_lfs_stat_fn   = stat_is_reg;
    mock_lfs_remove_fn = [](lfs_t *, const char *path) -> int {
        s_removed = path;
        return 0;
    };

    /* Longest name that still fits: the buffer holds the leading '/', the NUL,
     * and one more byte for the trailing separator the normaliser writes after
     * every component before stripping it again. */
    const std::string name(FTP_SERVER_PATH_MAX - 3, 'a');
    REQUIRE(send_cmd(c, "DELE /" + name + "\r\n") ==
            "250 Delete operation successful.\r\n");
    REQUIRE(s_removed == "/" + name);
}

TEST_CASE("an over-long relative path is rejected too", "[path]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_reg;
    const std::string too_long(FTP_SERVER_PATH_MAX + 8, 'a');
    REQUIRE(send_cmd(c, "DELE " + too_long + "\r\n") == "550 Path too long.\r\n");
}

TEST_CASE("an absolute path over-long only before normalising is accepted",
          "[path]")
{
    /* The length limit applies to the *resolved* path, and is enforced one
     * component at a time. "/<dir>/../a.txt" is longer than the buffer as
     * written, yet every component it keeps fits and it names a path of two
     * bytes plus a name — rejecting it would be a false positive.
     *
     * (A single component that cannot itself be buffered is still rejected,
     * even if a following ".." would have dropped it again; the normaliser
     * materialises each component before it can know that.) */
    init_server();
    Client c = connect_client();

    static std::string s_removed;
    s_removed.clear();
    mock_lfs_stat_fn   = stat_is_reg;
    mock_lfs_remove_fn = [](lfs_t *, const char *path) -> int {
        s_removed = path;
        return 0;
    };

    /* Longest component that still fits on its own (see the "path that fits"
     * case above), which makes "/<dir>/../a.txt" overflow only as written. */
    const std::string dir(FTP_SERVER_PATH_MAX - 3, 'd');
    const std::string arg = "/" + dir + "/../a.txt";
    REQUIRE(arg.size() > FTP_SERVER_PATH_MAX);

    REQUIRE(send_cmd(c, "DELE " + arg + "\r\n") ==
            "250 Delete operation successful.\r\n");
    REQUIRE(s_removed == "/a.txt");
}
