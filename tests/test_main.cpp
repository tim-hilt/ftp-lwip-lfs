/**
 * @file test_main.cpp
 * @brief Catch2 unit tests for the FTP server built without credentials
 *        (FTP_SERVER_USER == NULL -> every client is logged in on accept).
 *
 * The credential paths are compile-time excluded from this build and are
 * covered separately by tests/test_auth.cpp. See ftp_test_support.hpp for
 * how the lwIP callbacks are driven.
 */
#include "ftp_test_support.hpp"

#include <cstdio>

using namespace ftptest;

/* ==================================================================== */
/*  Server lifecycle                                                    */
/* ==================================================================== */

TEST_CASE("ftp_server_init rejects a NULL filesystem", "[init]")
{
    mock_lwip_reset();
    mock_lfs_reset();
    REQUIRE(ftp_server_init(NULL) == ERR_ARG);
}

TEST_CASE("ftp_server_init rejects an oversized lfs cache_size", "[init]")
{
    mock_lwip_reset();
    mock_lfs_reset();
    mock_lfs_cfg.cache_size = FTP_SERVER_FILE_CACHE_SIZE + 1;

    static int s_tcp_new_calls;
    s_tcp_new_calls = 0;
    struct Counter {
        static struct tcp_pcb *fn(void) { s_tcp_new_calls++; return nullptr; }
    };
    mock_tcp_new_fn = Counter::fn;
    REQUIRE(ftp_server_init(&mock_lfs) == ERR_ARG);
    /* tcp_new() must never be reached — the cache check happens first. */
    REQUIRE(s_tcp_new_calls == 0);
    mock_tcp_new_fn = nullptr;
}

TEST_CASE("ftp_server_init succeeds and registers the accept callback", "[init]")
{
    init_server();
    REQUIRE(mock_tcp_cb_accept[kListenIdx] != nullptr);
}

TEST_CASE("ftp_server_init propagates a bind failure", "[init]")
{
    mock_lwip_reset();
    mock_lfs_reset();
    mock_tcp_bind_fn = [](struct tcp_pcb *, const ip_addr_t *, u16_t) -> err_t {
        return ERR_USE;
    };
    REQUIRE(ftp_server_init(&mock_lfs) == ERR_USE);
}

TEST_CASE("ftp_server_init frees the bound PCB when listen fails", "[init]")
{
    mock_lwip_reset();
    mock_lfs_reset();

    static struct tcp_pcb *s_closed;
    s_closed = nullptr;
    mock_tcp_close_fn = [](struct tcp_pcb *pcb) -> err_t { s_closed = pcb; return ERR_OK; };
    mock_tcp_listen_with_backlog_fn = [](struct tcp_pcb *, u8_t) -> struct tcp_pcb * {
        return nullptr;
    };

    REQUIRE(ftp_server_init(&mock_lfs) == ERR_MEM);
    /* tcp_listen() leaves the bound PCB allocated on failure — it must be
     * closed, not leaked. */
    REQUIRE(s_closed != nullptr);

    mock_tcp_close_fn = nullptr;
    mock_tcp_listen_with_backlog_fn = nullptr;
}

TEST_CASE("ftp_server_deinit closes sessions and is idempotent", "[init]")
{
    init_server();
    Client c = connect_client();

    ftp_server_deinit();

    /* The connected session's recv callback must be cleared (session torn down). */
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);

    /* Calling deinit again must not crash. */
    ftp_server_deinit();
}

/* ==================================================================== */
/*  Connection / greeting                                               */
/* ==================================================================== */

TEST_CASE("accepting a control connection sends the greeting", "[connect]")
{
    init_server();
    u16_t before = mock_tcp_write_len;
    Client c = connect_client();
    std::string reply = written_since(before);
    REQUIRE(reply == "220 LwIP-LFS FTP server ready.\r\n");
    (void)c;
}

TEST_CASE("beyond FTP_SERVER_MAX_CLIENTS the connection is aborted", "[connect]")
{
    init_server();

    for (int i = 0; i < FTP_SERVER_MAX_CLIENTS; i++) {
        Client c = connect_client();
        (void)c;
    }

    static struct tcp_pcb *s_aborted_pcb;
    s_aborted_pcb = nullptr;
    struct Hook {
        static void fn(struct tcp_pcb *pcb) { s_aborted_pcb = pcb; }
    };
    mock_tcp_abort_fn = Hook::fn;

    struct tcp_pcb *pcb = tcp_new();
    err_t err = mock_tcp_cb_accept[kListenIdx](mock_tcp_cb_arg[kListenIdx], pcb, ERR_OK);
    mock_tcp_abort_fn = nullptr;

    REQUIRE(err == ERR_ABRT);
    REQUIRE(s_aborted_pcb == pcb);
}

TEST_CASE("the accept callback rejects a failed connection", "[connect]")
{
    init_server();
    REQUIRE(mock_tcp_cb_accept[kListenIdx](mock_tcp_cb_arg[kListenIdx], nullptr, ERR_ABRT)
            == ERR_VAL);
}

TEST_CASE("client disconnect via recv NULL tears down the session", "[connect]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(mock_tcp_cb_recv[c.idx] != nullptr);

    /* Simulate TCP FIN: lwIP delivers recv(NULL). */
    err_t err = mock_tcp_cb_recv[c.idx](mock_tcp_cb_arg[c.idx], c.pcb, nullptr, ERR_OK);
    REQUIRE(err == ERR_OK);

    /* Session must be fully cleaned up. */
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);
}

TEST_CASE("a control recv on a detached PCB is a no-op", "[connect]")
{
    init_server();
    Client c = connect_client();

    /* lwIP can deliver one last segment after tcp_arg(pcb, NULL). */
    std::string line = "NOOP\r\n";
    struct pbuf p = make_pbuf(line);
    u16_t before = mock_tcp_write_len;
    REQUIRE(mock_tcp_cb_recv[c.idx](nullptr, c.pcb, &p, ERR_OK) == ERR_OK);
    REQUIRE(mock_tcp_write_len == before);
}

TEST_CASE("control error callback cleans up the session", "[connect]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(mock_tcp_cb_err[c.idx] != nullptr);

    /* lwIP calls the error callback (e.g. on RST) — PCB already freed. */
    mock_tcp_cb_err[c.idx](mock_tcp_cb_arg[c.idx], ERR_RST);
    /* A detached error callback must be harmless too. */
    mock_tcp_cb_err[c.idx](nullptr, ERR_RST);

    /* Session slot must be released so a new client can connect. */
    Client c2 = connect_client();
    (void)c2;
}

TEST_CASE("idle timeout closes the session after enough polls", "[connect]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(mock_tcp_cb_poll[c.idx] != nullptr);

    /* A detached poll callback must be harmless. */
    REQUIRE(mock_tcp_cb_poll[c.idx](nullptr, c.pcb) == ERR_OK);

    /* Fire poll up to (but not including) the limit — session stays alive. */
    for (int i = 0; i < FTP_SERVER_IDLE_TIMEOUT_POLLS - 1; i++) {
        err_t err = mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
        REQUIRE(err == ERR_OK);
        REQUIRE(mock_tcp_cb_recv[c.idx] != nullptr);
    }

    /* One more poll pushes over the limit. */
    mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);
}

TEST_CASE("receiving data resets the idle counter", "[connect]")
{
    init_server();
    Client c = connect_client();

    for (int i = 0; i < FTP_SERVER_IDLE_TIMEOUT_POLLS - 1; i++) {
        mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
    }
    REQUIRE(send_cmd(c, "NOOP\r\n") == "200 NOOP ok.\r\n");

    /* The counter restarted, so one more poll must not close the session. */
    mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
    REQUIRE(mock_tcp_cb_recv[c.idx] != nullptr);
}

/* ==================================================================== */
/*  lwIP callback contract: abort must surface as ERR_ABRT              */
/* ==================================================================== */

TEST_CASE("a close that degrades to abort returns ERR_ABRT to lwIP", "[lwip]")
{
    /* lwIP frees an aborted PCB immediately; a callback that keeps returning
     * ERR_OK would leave lwIP dereferencing freed memory. */
    init_server();
    Client c = connect_client();

    mock_tcp_close_fn = [](struct tcp_pcb *) -> err_t { return ERR_MEM; };
    mock_tcp_abort_fn = [](struct tcp_pcb *) {};

    SECTION("from the control recv callback (QUIT)") {
        REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
        REQUIRE(g_last_err == ERR_ABRT);
    }
    SECTION("from the control recv callback (client FIN)") {
        struct pbuf *none = nullptr;
        REQUIRE(mock_tcp_cb_recv[c.idx](mock_tcp_cb_arg[c.idx], c.pcb, none, ERR_OK)
                == ERR_ABRT);
    }
    SECTION("from the idle poll callback") {
        err_t err = ERR_OK;
        for (int i = 0; i < FTP_SERVER_IDLE_TIMEOUT_POLLS; i++) {
            err = mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
        }
        REQUIRE(err == ERR_ABRT);
    }

    mock_tcp_close_fn = nullptr;
    mock_tcp_abort_fn = nullptr;
}

TEST_CASE("an undeliverable reply tears the session down", "[lwip]")
{
    init_server();
    Client c = connect_client();

    /* Send window smaller than the reply -> ftp_send_reply gives up. */
    c.pcb->snd_buf = 4;
    REQUIRE(send_cmd(c, "NOOP\r\n").empty());
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);
}

TEST_CASE("a failing tcp_write on the control channel tears the session down", "[lwip]")
{
    init_server();
    Client c = connect_client();

    mock_tcp_write_fn = [](struct tcp_pcb *, const void *, u16_t, u8_t) -> err_t {
        return ERR_MEM;
    };
    REQUIRE(send_cmd(c, "NOOP\r\n").empty());
    mock_tcp_write_fn = nullptr;

    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
}

/* ==================================================================== */
/*  Simple post-auth commands (no auth configured -> logged in on accept)*/
/* ==================================================================== */

TEST_CASE("informational commands reply with expected content", "[commands]")
{
    init_server();
    Client c = connect_client();

    SECTION("SYST reports UNIX type") {
        REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
    }
    SECTION("FEAT lists supported features") {
        REQUIRE(send_cmd(c, "FEAT\r\n") ==
                "211-Features:\r\n"
                " PASV\r\n"
                " SIZE\r\n"
                " UTF8\r\n"
                "211 End\r\n");
    }
    SECTION("NOOP replies 200") {
        REQUIRE(send_cmd(c, "NOOP\r\n") == "200 NOOP ok.\r\n");
    }
    SECTION("OPTS UTF8 ON is accepted") {
        REQUIRE(send_cmd(c, "OPTS utf8 on\r\n") == "200 UTF8 set to on.\r\n");
    }
    SECTION("OPTS rejects unsupported options") {
        REQUIRE(send_cmd(c, "OPTS MLST\r\n") == "501 Option not understood.\r\n");
    }
    SECTION("OPTS rejects a missing argument") {
        REQUIRE(send_cmd(c, "OPTS\r\n") == "501 Option not understood.\r\n");
    }
    SECTION("unknown command returns 502") {
        REQUIRE(send_cmd(c, "FROB\r\n") == "502 Command not implemented.\r\n");
    }
    SECTION("commands are case-insensitive") {
        REQUIRE(send_cmd(c, "syst\r\n") == "215 UNIX Type: L8\r\n");
    }
}

TEST_CASE("TYPE command", "[commands]")
{
    init_server();
    Client c = connect_client();

    SECTION("accepts binary mode") {
        REQUIRE(send_cmd(c, "TYPE I\r\n") == "200 Switching to Binary mode.\r\n");
    }
    SECTION("rejects ASCII mode") {
        REQUIRE(send_cmd(c, "TYPE A\r\n") ==
                "504 ASCII transfer mode not supported, use binary.\r\n");
    }
    SECTION("rejects missing argument") {
        REQUIRE(send_cmd(c, "TYPE\r\n") ==
                "504 ASCII transfer mode not supported, use binary.\r\n");
    }
}

TEST_CASE("MODE and STRU accept their RFC 959 default values", "[commands]")
{
    /* RFC 959 section 5.1 lists MODE and STRU in the minimum implementation
     * every server must accept "for the default values" — Stream and File.
     * Clients that send them defensively must not get a 502. */
    init_server();
    Client c = connect_client();

    SECTION("MODE S is accepted") {
        REQUIRE(send_cmd(c, "MODE S\r\n") == "200 Mode set to S.\r\n");
        REQUIRE(send_cmd(c, "mode s\r\n") == "200 Mode set to S.\r\n");
    }
    SECTION("other transfer modes are refused") {
        for (const char *cmd : {"MODE B\r\n", "MODE C\r\n", "MODE\r\n",
                                "MODE Stream\r\n"}) {
            REQUIRE(send_cmd(c, cmd) == "504 Only stream mode is supported.\r\n");
        }
    }
    SECTION("STRU F is accepted") {
        REQUIRE(send_cmd(c, "STRU F\r\n") == "200 Structure set to F.\r\n");
        REQUIRE(send_cmd(c, "stru f\r\n") == "200 Structure set to F.\r\n");
    }
    SECTION("other structures are refused") {
        for (const char *cmd : {"STRU R\r\n", "STRU P\r\n", "STRU\r\n",
                                "STRU File\r\n"}) {
            REQUIRE(send_cmd(c, cmd) == "504 Only file structure is supported.\r\n");
        }
    }
}

TEST_CASE("QUIT replies and tears down the session", "[commands]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);
}

TEST_CASE("multiple commands in one TCP segment are all processed", "[commands]")
{
    init_server();
    Client c = connect_client();

    std::string out = send_cmd(c, "SYST\r\nNOOP\r\nPWD\r\n");
    REQUIRE(out == "215 UNIX Type: L8\r\n"
                   "200 NOOP ok.\r\n"
                   "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("a command split across two TCP segments is reassembled", "[commands]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "SY").empty()); /* partial — no reply yet */
    REQUIRE(send_cmd(c, "ST\r\n") == "215 UNIX Type: L8\r\n");
}

TEST_CASE("an unterminated line exceeding the buffer is discarded", "[commands]")
{
    init_server();
    Client c = connect_client();

    std::string huge(FTP_SERVER_CMD_BUF_SIZE + 32, 'A');
    REQUIRE(send_cmd(c, huge) == "500 Command line too long.\r\n");
}

TEST_CASE("the tail of an over-long line is not parsed as a command", "[commands]")
{
    init_server();
    Client c = connect_client();

    /* Overflow the buffer with the start of a bogus command. */
    std::string huge(FTP_SERVER_CMD_BUF_SIZE + 32, 'A');
    REQUIRE(send_cmd(c, huge) == "500 Command line too long.\r\n");

    /* More of the same line arrives, still without a terminator: silently
     * discarded, not answered with a second 500. */
    REQUIRE(send_cmd(c, std::string(64, 'A')).empty());

    /* The line finally ends. Everything up to the '\n' belongs to the
     * discarded command, so only the NOOP that follows is executed. */
    REQUIRE(send_cmd(c, "SYST\r\nNOOP\r\n") == "200 NOOP ok.\r\n");
}

/* ==================================================================== */
/*  Authentication when no credentials are configured                   */
/*  (the configured-credentials flow lives in test_auth.cpp)            */
/* ==================================================================== */

TEST_CASE("USER logs in immediately when no auth is configured", "[auth]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "USER anyone\r\n") == "230 Login successful.\r\n");
    /* Even a bare USER succeeds — the name is not consulted at all. */
    REQUIRE(send_cmd(c, "USER\r\n") == "230 Login successful.\r\n");
    REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
}

TEST_CASE("PASS without USER first is rejected", "[auth]")
{
    init_server();
    Client c = connect_client();

    /* Without credentials the client is already logged in, so no PASS is
     * awaited -> 503. */
    REQUIRE(send_cmd(c, "PASS whatever\r\n") == "503 Login with USER first.\r\n");
    REQUIRE(send_cmd(c, "USER anyone\r\n") == "230 Login successful.\r\n");
    REQUIRE(send_cmd(c, "PASS whatever\r\n") == "503 Login with USER first.\r\n");
}

/* ==================================================================== */
/*  Filesystem navigation                                               */
/* ==================================================================== */

TEST_CASE("PWD reports the root directory initially", "[fs]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("CWD into a directory updates PWD", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    REQUIRE(send_cmd(c, "CWD sub\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/sub\" is the current directory.\r\n");

    REQUIRE(send_cmd(c, "CDUP\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("CWD accepts an absolute path", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    REQUIRE(send_cmd(c, "CWD deep/nest\r\n") == "250 Directory successfully changed.\r\n");
    /* An absolute argument replaces the CWD rather than extending it. */
    REQUIRE(send_cmd(c, "CWD /other\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/other\" is the current directory.\r\n");
}

TEST_CASE("CWD to a non-directory or missing path fails", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_missing;
    REQUIRE(send_cmd(c, "CWD missing\r\n") == "550 Failed to change directory.\r\n");
    /* cwd is unchanged. */
    mock_lfs_stat_fn = nullptr;
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("relative path resolution collapses . and ..", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    REQUIRE(send_cmd(c, "CWD a/./b/../c\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/a/c\" is the current directory.\r\n");
}

TEST_CASE("a path that only fits once normalised is accepted", "[fs]")
{
    /* ".." is resolved while the path is being built rather than after
     * concatenating cwd and argument, so only the *result* has to fit in
     * FTP_SERVER_PATH_MAX. Rejecting on the intermediate length would refuse
     * a perfectly short destination. */
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    /* A cwd long enough that cwd + "/" + "../short" overflows the buffer. */
    std::string long_name(FTP_SERVER_CMD_BUF_SIZE - 4 - 2 - 1, 'x');
    REQUIRE(send_cmd(c, "CWD " + long_name + "\r\n") ==
            "250 Directory successfully changed.\r\n");
    REQUIRE(1 + long_name.size() + 1 + strlen("../short") >= FTP_SERVER_PATH_MAX);

    REQUIRE(send_cmd(c, "CWD ../short\r\n") ==
            "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/short\" is the current directory.\r\n");
}

TEST_CASE("CDUP at the root stays at the root", "[fs]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "CDUP\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("a resolved path that overflows FTP_SERVER_PATH_MAX is rejected", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    /* Build a cwd close to FTP_SERVER_PATH_MAX in one hop (must itself fit
     * within FTP_SERVER_CMD_BUF_SIZE as a command line). */
    std::string long_name(FTP_SERVER_CMD_BUF_SIZE - 4 - 2 - 1, 'x');
    REQUIRE(send_cmd(c, "CWD " + long_name + "\r\n") ==
            "250 Directory successfully changed.\r\n");

    /* A short additional component now pushes the resolved path over
     * FTP_SERVER_PATH_MAX. */
    REQUIRE(send_cmd(c, "CWD short\r\n") == "550 Path too long.\r\n");

    /* cwd must be unchanged by the failed CWD. */
    mock_lfs_stat_fn = nullptr;
    std::string pwd = send_cmd(c, "PWD\r\n");
    REQUIRE(pwd.rfind("257 \"/" + long_name + "\"", 0) == 0);
}

TEST_CASE("every path-taking command rejects an over-long path", "[fs]")
{
    /* One shared guard, so a new command that forgets the check is caught. */
    const char *commands[] = {
        "CWD", "DELE", "MKD", "RMD", "RNFR", "SIZE", "RETR", "STOR",
        "LIST", "NLST",
    };

    for (const char *cmd : commands) {
        init_server();
        Client c = connect_client();
        mock_lfs_stat_fn = stat_is_dir;

        std::string long_name(FTP_SERVER_CMD_BUF_SIZE - 4 - 2 - 1, 'x');
        REQUIRE(send_cmd(c, "CWD " + long_name + "\r\n") ==
                "250 Directory successfully changed.\r\n");

        INFO("command: " << cmd);
        REQUIRE(send_cmd(c, std::string(cmd) + " short\r\n") == "550 Path too long.\r\n");
    }

    /* RNTO resolves its path before consulting the pending RNFR. */
    init_server();
    Client c = connect_client();
    mock_lfs_stat_fn = stat_is_dir;
    std::string long_name(FTP_SERVER_CMD_BUF_SIZE - 4 - 2 - 1, 'x');
    REQUIRE(send_cmd(c, "CWD " + long_name + "\r\n") ==
            "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "RNTO short\r\n") == "550 Path too long.\r\n");
}

/* ==================================================================== */
/*  X-aliases (XPWD, XCWD, XCUP, XMKD, XRMD)                          */
/* ==================================================================== */

TEST_CASE("X-aliases map to their standard counterparts", "[commands]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = stat_is_dir;

    SECTION("XPWD behaves like PWD") {
        REQUIRE(send_cmd(c, "XPWD\r\n") == "257 \"/\" is the current directory.\r\n");
    }
    SECTION("XCWD behaves like CWD") {
        REQUIRE(send_cmd(c, "XCWD sub\r\n") == "250 Directory successfully changed.\r\n");
        REQUIRE(send_cmd(c, "XPWD\r\n") == "257 \"/sub\" is the current directory.\r\n");
    }
    SECTION("XCUP behaves like CDUP") {
        REQUIRE(send_cmd(c, "XCWD sub\r\n") == "250 Directory successfully changed.\r\n");
        REQUIRE(send_cmd(c, "XCUP\r\n") == "250 Directory successfully changed.\r\n");
        REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
    }
    SECTION("XMKD behaves like MKD") {
        REQUIRE(send_cmd(c, "XMKD newdir\r\n") == "257 \"/newdir\" created.\r\n");
    }
    SECTION("XRMD behaves like RMD") {
        REQUIRE(send_cmd(c, "XRMD olddir\r\n") == "250 Remove directory successful.\r\n");
    }
}

/* ==================================================================== */
/*  DELE / MKD / RMD / RNFR / RNTO / SIZE                              */
/* ==================================================================== */

TEST_CASE("MKD creates a directory or reports failure", "[fs]")
{
    init_server();
    Client c = connect_client();

    SECTION("creates directory successfully") {
        REQUIRE(send_cmd(c, "MKD newdir\r\n") == "257 \"/newdir\" created.\r\n");
    }
    SECTION("reports failure when mkdir fails") {
        mock_lfs_mkdir_fn = [](lfs_t *, const char *) -> int { return -1; };
        REQUIRE(send_cmd(c, "MKD newdir\r\n") == "550 Create directory failed.\r\n");
    }
    SECTION("rejects missing argument") {
        REQUIRE(send_cmd(c, "MKD\r\n") == "501 Specify directory name.\r\n");
    }
}

TEST_CASE("RMD removes a directory or reports failure", "[fs]")
{
    init_server();
    Client c = connect_client();
    mock_lfs_stat_fn = stat_is_dir;

    SECTION("removes directory successfully") {
        REQUIRE(send_cmd(c, "RMD olddir\r\n") == "250 Remove directory successful.\r\n");
    }
    SECTION("reports failure when remove fails") {
        mock_lfs_remove_fn = [](lfs_t *, const char *) -> int { return -1; };
        REQUIRE(send_cmd(c, "RMD olddir\r\n") == "550 Remove directory failed.\r\n");
    }
    SECTION("refuses to remove a regular file") {
        mock_lfs_stat_fn = stat_is_reg;
        REQUIRE(send_cmd(c, "RMD notadir\r\n") == "550 Remove directory failed.\r\n");
    }
    SECTION("reports failure for a missing path") {
        mock_lfs_stat_fn = stat_missing;
        REQUIRE(send_cmd(c, "RMD gone\r\n") == "550 Remove directory failed.\r\n");
    }
    SECTION("rejects missing argument") {
        REQUIRE(send_cmd(c, "RMD\r\n") == "501 Specify directory name.\r\n");
    }
}

TEST_CASE("DELE deletes a file or reports failure", "[fs]")
{
    init_server();
    Client c = connect_client();

    SECTION("deletes file successfully") {
        REQUIRE(send_cmd(c, "DELE file.txt\r\n") == "250 Delete operation successful.\r\n");
    }
    SECTION("reports failure when remove fails") {
        mock_lfs_remove_fn = [](lfs_t *, const char *) -> int { return -1; };
        REQUIRE(send_cmd(c, "DELE file.txt\r\n") == "550 Delete operation failed.\r\n");
    }
    SECTION("refuses to delete a directory") {
        mock_lfs_stat_fn = stat_is_dir;
        REQUIRE(send_cmd(c, "DELE adir\r\n") == "550 Delete operation failed.\r\n");
    }
    SECTION("rejects missing argument") {
        REQUIRE(send_cmd(c, "DELE\r\n") == "501 Specify file name.\r\n");
    }
}

TEST_CASE("an empty argument is not the current directory", "[fs][security]")
{
    /* Given a command line with a trailing space, the argument parses to ""
     * rather than to no argument at all. Resolving "" yields the CWD, so
     * `RMD ` used to delete the directory the client was sitting in and
     * answer 250. An empty argument must be treated as a missing one. */
    init_server();
    Client c = connect_client();

    static int s_removes;
    s_removes = 0;
    mock_lfs_remove_fn = [](lfs_t *, const char *) -> int { s_removes++; return 0; };

    mock_lfs_stat_fn = stat_is_dir;
    REQUIRE(send_cmd(c, "CWD /sub\r\n") == "250 Directory successfully changed.\r\n");

    SECTION("RMD does not remove the working directory") {
        REQUIRE(send_cmd(c, "RMD \r\n")   == "501 Specify directory name.\r\n");
        REQUIRE(send_cmd(c, "RMD   \r\n") == "501 Specify directory name.\r\n");
    }
    SECTION("DELE does not remove the working directory") {
        mock_lfs_stat_fn = stat_is_reg;
        REQUIRE(send_cmd(c, "DELE \r\n") == "501 Specify file name.\r\n");
    }
    SECTION("every other path command reports the missing argument") {
        for (const char *cmd : {"MKD \r\n", "RETR \r\n", "STOR \r\n",
                                "SIZE \r\n", "RNFR \r\n", "RNTO \r\n"}) {
            INFO("command: " << cmd);
            REQUIRE(send_cmd(c, cmd).rfind("501 Specify", 0) == 0);
        }
    }

    /* Nothing was unlinked, and the working directory still stands. */
    REQUIRE(s_removes == 0);
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/sub\" is the current directory.\r\n");
}

TEST_CASE("RNFR / RNTO rename flow", "[fs]")
{
    init_server();
    Client c = connect_client();

    SECTION("RNTO without RNFR is rejected") {
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "503 RNFR required first.\r\n");
    }

    SECTION("RNFR on a missing file fails") {
        mock_lfs_stat_fn = stat_missing;
        REQUIRE(send_cmd(c, "RNFR missing.txt\r\n") == "550 File not found.\r\n");
    }

    SECTION("RNFR then RNTO renames") {
        REQUIRE(send_cmd(c, "RNFR a.txt\r\n") == "350 Ready for RNTO.\r\n");
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "250 Rename successful.\r\n");
    }

    SECTION("rename failure is reported") {
        REQUIRE(send_cmd(c, "RNFR a.txt\r\n") == "350 Ready for RNTO.\r\n");
        mock_lfs_rename_fn = [](lfs_t *, const char *, const char *) -> int { return -1; };
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "550 Rename failed.\r\n");
    }

    SECTION("an intervening command cancels the pending rename") {
        /* RFC 959: RNTO must directly follow RNFR. */
        REQUIRE(send_cmd(c, "RNFR a.txt\r\n") == "350 Ready for RNTO.\r\n");
        REQUIRE(send_cmd(c, "NOOP\r\n") == "200 NOOP ok.\r\n");
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "503 RNFR required first.\r\n");
    }

    SECTION("an intervening *unknown* command cancels it too") {
        /* A command with no dispatch entry still passed through the auth
         * gate, so it counts as "something happened in between". */
        REQUIRE(send_cmd(c, "RNFR a.txt\r\n") == "350 Ready for RNTO.\r\n");
        REQUIRE(send_cmd(c, "FROB\r\n") == "502 Command not implemented.\r\n");
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "503 RNFR required first.\r\n");
    }

    SECTION("RNFR rejects a missing argument") {
        REQUIRE(send_cmd(c, "RNFR\r\n") == "501 Specify file name.\r\n");
    }
    SECTION("RNTO rejects a missing argument") {
        REQUIRE(send_cmd(c, "RNTO\r\n") == "501 Specify file name.\r\n");
    }
}

TEST_CASE("SIZE reports file size or rejects non-files", "[fs]")
{
    init_server();
    Client c = connect_client();

    SECTION("reports size for a regular file") {
        mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
            if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_REG; info->size = 12345; }
            return 0;
        };
        REQUIRE(send_cmd(c, "SIZE file.bin\r\n") == "213 12345\r\n");
    }
    SECTION("rejects directories") {
        mock_lfs_stat_fn = stat_is_dir;
        REQUIRE(send_cmd(c, "SIZE adir\r\n") == "550 Could not get file size.\r\n");
    }
    SECTION("rejects missing argument") {
        REQUIRE(send_cmd(c, "SIZE\r\n") == "501 Specify file name.\r\n");
    }
}

/* ==================================================================== */
/*  PASV / PORT                                                         */
/* ==================================================================== */

TEST_CASE("PASV reports the listener's port, incrementing across calls", "[pasv]")
{
    init_server();
    Client c = connect_client();

    std::string r1 = send_cmd(c, "PASV\r\n");
    char expect1[64];
    std::snprintf(expect1, sizeof(expect1),
                  "227 Entering Passive Mode (192,168,1,1,%u,%u).\r\n",
                  (unsigned)(FTP_SERVER_PASV_PORT_MIN >> 8),
                  (unsigned)(FTP_SERVER_PASV_PORT_MIN & 0xff));
    REQUIRE(r1 == expect1);

    std::string r2 = send_cmd(c, "PASV\r\n");
    char expect2[64];
    std::snprintf(expect2, sizeof(expect2),
                  "227 Entering Passive Mode (192,168,1,1,%u,%u).\r\n",
                  (unsigned)((FTP_SERVER_PASV_PORT_MIN + 1) >> 8),
                  (unsigned)((FTP_SERVER_PASV_PORT_MIN + 1) & 0xff));
    REQUIRE(r2 == expect2);
}

TEST_CASE("the PASV port allocator wraps back to the first port", "[pasv]")
{
    init_server();
    Client c = connect_client();

    constexpr int kRange = FTP_SERVER_PASV_PORT_MAX - FTP_SERVER_PASV_PORT_MIN + 1;
    for (int i = 0; i < kRange; i++) {
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    }

    char expect[64];
    std::snprintf(expect, sizeof(expect),
                  "227 Entering Passive Mode (192,168,1,1,%u,%u).\r\n",
                  (unsigned)(FTP_SERVER_PASV_PORT_MIN >> 8),
                  (unsigned)(FTP_SERVER_PASV_PORT_MIN & 0xff));
    REQUIRE(send_cmd(c, "PASV\r\n") == expect);
}

TEST_CASE("PASV reports socket setup failures", "[pasv]")
{
    init_server();
    Client c = connect_client();

    SECTION("no PCB available") {
        mock_tcp_new_fn = []() -> struct tcp_pcb * { return nullptr; };
        REQUIRE(send_cmd(c, "PASV\r\n") == "421 Cannot create data socket.\r\n");
        mock_tcp_new_fn = nullptr;
    }

    SECTION("every port in the range is in use") {
        static int s_binds;
        s_binds = 0;
        mock_tcp_bind_fn = [](struct tcp_pcb *, const ip_addr_t *, u16_t) -> err_t {
            s_binds++;
            return ERR_USE;
        };
        REQUIRE(send_cmd(c, "PASV\r\n") == "421 Cannot bind data socket.\r\n");
        mock_tcp_bind_fn = nullptr;
        /* Every port in the configured range must have been tried. */
        REQUIRE(s_binds == FTP_SERVER_PASV_PORT_MAX - FTP_SERVER_PASV_PORT_MIN + 1);
    }

    SECTION("listen fails and the bound PCB is freed") {
        static struct tcp_pcb *s_closed;
        s_closed = nullptr;
        mock_tcp_close_fn = [](struct tcp_pcb *pcb) -> err_t { s_closed = pcb; return ERR_OK; };
        mock_tcp_listen_with_backlog_fn = [](struct tcp_pcb *, u8_t) -> struct tcp_pcb * {
            return nullptr;
        };
        REQUIRE(send_cmd(c, "PASV\r\n") == "421 Cannot listen on data socket.\r\n");
        REQUIRE(s_closed != nullptr);
        mock_tcp_close_fn = nullptr;
        mock_tcp_listen_with_backlog_fn = nullptr;
    }
}

TEST_CASE("PORT parses a valid h1,h2,h3,h4,p1,p2 argument", "[port]")
{
    init_server();
    Client c = connect_client();
    /* The mock reports 192.168.1.1 as the control connection's peer. */
    REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,1\r\n") == "200 PORT command successful.\r\n");
}

TEST_CASE("PORT refuses an address other than the client's", "[port][security]")
{
    /* RFC 2577: allowing an arbitrary address turns the server into an
     * "FTP bounce" proxy for scanning or relaying to third-party hosts. */
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "PORT 10,0,0,7,4,1\r\n") ==
            "501 PORT address must match the control connection.\r\n");

    /* And no data channel was armed by the rejected command. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("PORT rejects malformed arguments", "[port]")
{
    init_server();
    Client c = connect_client();

    SECTION("non-numeric input") {
        REQUIRE(send_cmd(c, "PORT garbage\r\n") == "501 Syntax error in parameters.\r\n");
    }
    SECTION("too few components") {
        REQUIRE(send_cmd(c, "PORT 10,0,0,1,4\r\n") == "501 Syntax error in parameters.\r\n");
    }
    SECTION("octet value out of range") {
        REQUIRE(send_cmd(c, "PORT 300,0,0,1,4,1\r\n") == "501 Syntax error in parameters.\r\n");
    }
    SECTION("trailing garbage after the last field") {
        REQUIRE(send_cmd(c, "PORT 10,0,0,1,4,1x\r\n") == "501 Syntax error in parameters.\r\n");
    }
    SECTION("missing argument") {
        REQUIRE(send_cmd(c, "PORT\r\n") == "501 Syntax error in parameters.\r\n");
    }
}

TEST_CASE("LIST/RETR before PASV or PORT fails", "[transfer]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
    /* The aborted LIST must not leave the directory handle open. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

/* ==================================================================== */
/*  LIST / NLST                                                         */
/* ==================================================================== */

namespace {

/** lfs_dir_read hook yielding ".", "..", "foo.txt" (42 B), "sub" (dir). */
int dir_read_two_entries(lfs_t *, lfs_dir_t *, struct lfs_info *info)
{
    static const struct { const char *name; uint8_t type; uint32_t size; } kEntries[] = {
        {".",       LFS_TYPE_DIR, 0},
        {"..",      LFS_TYPE_DIR, 0},
        {"foo.txt", LFS_TYPE_REG, 42},
        {"sub",     LFS_TYPE_DIR, 0},
    };
    static size_t call = 0;
    if (info == nullptr) { call = 0; return 0; } /* reset hook */
    if (call >= sizeof(kEntries) / sizeof(kEntries[0])) return 0;
    std::memset(info, 0, sizeof(*info));
    info->type = kEntries[call].type;
    info->size = kEntries[call].size;
    std::strcpy(info->name, kEntries[call].name);
    call++;
    return 1;
}

} // namespace

TEST_CASE("LIST streams directory entries in Unix format", "[transfer]")
{
    init_server();
    Client c = connect_client();

    dir_read_two_entries(nullptr, nullptr, nullptr); /* reset */
    mock_lfs_dir_read_fn = dir_read_two_entries;

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "LIST\r\n").empty()); /* async: waits for data conn */

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    REQUIRE(out.rfind("150 Here comes the data.\r\n", 0) == 0);

    /* Verify Unix-style listing format: permission string, owner, size, name. */
    REQUIRE(out.find("-rw-r--r-- 1 ftp ftp         42 Jan 01  2000 foo.txt\r\n") != std::string::npos);
    REQUIRE(out.find("drwxr-xr-x 1 ftp ftp          0 Jan 01  2000 sub\r\n") != std::string::npos);

    /* "." and ".." are filtered out of the listing. */
    REQUIRE(out.find(" .\r\n") == std::string::npos);
    REQUIRE(out.find(" ..\r\n") == std::string::npos);

    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("LIST strips ls-style option arguments", "[transfer]")
{
    init_server();
    Client c = connect_client();

    static std::string s_opened;
    s_opened.clear();
    mock_lfs_dir_open_fn = [](lfs_t *, lfs_dir_t *, const char *path) -> int {
        s_opened = path;
        return 0;
    };

    SECTION("a bare option lists the current directory") {
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "LIST -la\r\n").empty());
        REQUIRE(s_opened == "/");
    }
    SECTION("an option followed by a path lists that path") {
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "LIST -la sub\r\n").empty());
        REQUIRE(s_opened == "/sub");
    }
    SECTION("several separate options are all skipped") {
        /* Stopping after the first option would treat "-a" as a path. */
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "LIST -l -a sub\r\n").empty());
        REQUIRE(s_opened == "/sub");
    }
    SECTION("options with no path still list the current directory") {
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "LIST -l -a\r\n").empty());
        REQUIRE(s_opened == "/");
    }
}

TEST_CASE("LIST fails when directory cannot be opened", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_dir_open_fn = [](lfs_t *, lfs_dir_t *, const char *) -> int { return -1; };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "LIST\r\n") == "550 Failed to open directory.\r\n");
}

TEST_CASE("a directory read error aborts the listing with 451", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_dir_read_fn = [](lfs_t *, lfs_dir_t *, struct lfs_info *) -> int {
        return -1; /* e.g. LFS_ERR_CORRUPT */
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "LIST\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    /* A read error must not be reported to the client as success. */
    REQUIRE(out.find("226 Transfer complete.") == std::string::npos);
    REQUIRE(out.find("451 Requested action aborted") != std::string::npos);
}

TEST_CASE("NLST lists only bare filenames", "[transfer]")
{
    init_server();
    Client c = connect_client();

    dir_read_two_entries(nullptr, nullptr, nullptr); /* reset */
    mock_lfs_dir_read_fn = dir_read_two_entries;

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "NLST\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    /* Strip the 150 and 226 control-channel replies to isolate data. */
    auto data_start = out.find('\n', out.find("150 "));
    REQUIRE(data_start != std::string::npos);
    data_start++;
    auto data_end = out.find("226 Transfer complete.\r\n");
    REQUIRE(data_end != std::string::npos);
    std::string data_payload = out.substr(data_start, data_end - data_start);

    /* NLST must emit only "name\r\n" — no permission string, no size. */
    REQUIRE(data_payload == "foo.txt\r\nsub\r\n");
}

TEST_CASE("a listing entry longer than the data buffer is truncated cleanly",
          "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_dir_read_fn = [](lfs_t *, lfs_dir_t *, struct lfs_info *info) -> int {
        static int call = 0;
        if (info == nullptr) { call = 0; return 0; }
        if (call++ > 0) return 0;
        std::memset(info, 0, sizeof(*info));
        info->type = LFS_TYPE_REG;
        info->size = 1;
        std::memset(info->name, 'n', sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        return 1;
    };
    mock_lfs_dir_read_fn(nullptr, nullptr, nullptr); /* reset */

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "NLST\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    /* snprintf reserves the last buffer slot for its NUL; sending that slot
     * would put a stray NUL byte on the wire. */
    REQUIRE(out.find('\0') == std::string::npos);
}

/* ==================================================================== */
/*  RETR                                                                 */
/* ==================================================================== */

namespace {

/** lfs_file_read hook yielding `text` once, then EOF. */
const char *g_retr_text = "";
lfs_ssize_t read_once(lfs_t *, lfs_file_t *, void *buffer, lfs_size_t size)
{
    static bool done = false;
    if (buffer == nullptr) { done = false; return 0; } /* reset hook */
    if (done) return 0;
    done = true;
    lfs_size_t n = (lfs_size_t)std::strlen(g_retr_text);
    if (n > size) n = size;
    std::memcpy(buffer, g_retr_text, n);
    return (lfs_ssize_t)n;
}

} // namespace

TEST_CASE("RETR sends file content over the data connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 11; };
    g_retr_text = "hello world";
    read_once(nullptr, nullptr, nullptr, 0); /* reset */
    mock_lfs_file_read_fn = read_once;

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR file.txt\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    /* Only binary transfers are implemented, so the mode is always BINARY.
     * The byte count matters — clients use it for progress reporting. */
    REQUIRE(out.rfind("150 Opening BINARY mode data connection (11 bytes).\r\n", 0) == 0);
    REQUIRE(out.find("hello world") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("RETR resumes when the send window fills up", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 11; };
    g_retr_text = "hello world";
    read_once(nullptr, nullptr, nullptr, 0); /* reset */
    mock_lfs_file_read_fn = read_once;

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR file.txt\r\n").empty());

    /* Data socket accepts only 5 of the 11 bytes before blocking. */
    mock_tcp_track_sndbuf = 1;
    u16_t before = mock_tcp_write_len;
    struct tcp_pcb *data_pcb = accept_pasv_data_connection(/*snd_buf=*/5);
    std::string first = written_since(before);

    REQUIRE(first.find("hello") != std::string::npos);
    REQUIRE(first.find(" world") == std::string::npos);
    REQUIRE(first.find("226 Transfer complete.\r\n") == std::string::npos);

    /* The peer ACKs; lwIP invokes the sent callback and the rest goes out. */
    int data_idx = mock_tcp_pcb_index(data_pcb);
    REQUIRE(mock_tcp_cb_sent[data_idx] != nullptr);
    mock_tcp_ack(data_pcb, 64);

    before = mock_tcp_write_len;
    err_t err = mock_tcp_cb_sent[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, 5);
    std::string rest = written_since(before);
    mock_tcp_track_sndbuf = 0;

    REQUIRE(err == ERR_OK);
    REQUIRE(rest.rfind(" world", 0) == 0);
    REQUIRE(rest.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("a detached data sent callback is a no-op", "[transfer]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    u16_t before = mock_tcp_write_len;
    REQUIRE(mock_tcp_cb_sent[data_idx](nullptr, data_pcb, 0) == ERR_OK);
    /* STOR is an upload — the sent callback has nothing to push either. */
    REQUIRE(mock_tcp_cb_sent[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, 0) == ERR_OK);
    REQUIRE(mock_tcp_write_len == before);
}

TEST_CASE("a data connection reset aborts the transfer with 426", "[transfer]")
{
    init_server();
    Client c = connect_client();

    /* An upload stays open across callbacks, so the data PCB is still live
     * when the peer resets the connection. */
    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);
    REQUIRE(mock_tcp_cb_err[data_idx] != nullptr);

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_err[data_idx](mock_tcp_cb_arg[data_idx], ERR_RST);
    /* A detached error callback must be harmless. */
    mock_tcp_cb_err[data_idx](nullptr, ERR_RST);
    std::string out = written_since(before);

    /* Without a 426 the client waits forever for a 226 that never comes. */
    REQUIRE(out == "426 Connection closed; transfer aborted.\r\n");

    /* The reset must not leave the session unable to transfer again. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("RETR reports a file-size failure rather than announcing it", "[transfer]")
{
    /* An unchecked lfs_file_size() error reaches the client as a negative
     * byte count in the "150" reply. */
    init_server();
    Client c = connect_client();

    static int s_closes;
    s_closes = 0;
    mock_lfs_file_close_fn = [](lfs_t *, lfs_file_t *) -> int { s_closes++; return 0; };
    mock_lfs_file_size_fn  = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return -1; };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR file.bin\r\n") ==
            "451 Requested action aborted: local error in processing.\r\n");

    /* The file opened for the transfer must not be left dangling — a second
     * open of the same handle corrupts littlefs's list of open files. */
    REQUIRE(s_closes == 1);
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("ABOR reports the transfer it cut short", "[transfer]")
{
    init_server();
    Client c = connect_client();

    SECTION("with nothing running, only the 226 is sent") {
        REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");
    }

    SECTION("a transfer that never announced itself is dropped silently") {
        /* Armed, but the data connection is not up, so no 150 went out and
         * the client is not waiting on a completion reply. */
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "RETR a.txt\r\n").empty());
        REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");
    }

    SECTION("a live transfer gets its 426 before the 226") {
        REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
        REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
        accept_pasv_data_connection();   /* sends the "150" */
        REQUIRE(send_cmd(c, "ABOR\r\n") ==
                "426 Connection closed; transfer aborted.\r\n"
                "226 ABOR command successful.\r\n");
    }
}

TEST_CASE("a new PASV or PORT reports the transfer it supersedes", "[transfer]")
{
    /* Both commands drop the current data channel. Without the 426 the client
     * is left waiting for a 226 that the superseded transfer will never send. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    accept_pasv_data_connection();

    SECTION("PASV") {
        std::string out = send_cmd(c, "PASV\r\n");
        REQUIRE(out.rfind("426 Connection closed; transfer aborted.\r\n", 0) == 0);
        REQUIRE(out.find("227 Entering Passive Mode") != std::string::npos);
    }
    SECTION("PORT") {
        REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") ==
                "426 Connection closed; transfer aborted.\r\n"
                "200 PORT command successful.\r\n");
    }
}

TEST_CASE("an active-mode connect that fails reports 425, not 426", "[transfer]")
{
    /* lwIP reports a failed connect through the error callback rather than
     * the connected callback. A connection that never opened is a 425; 426
     * would tell the client a transfer died in flight. */
    init_server();
    Client c = connect_client();

    static struct tcp_pcb *s_pcb;
    s_pcb = nullptr;
    mock_tcp_connect_fn = [](struct tcp_pcb *pcb, const ip_addr_t *, u16_t,
                             tcp_connected_fn) -> err_t { s_pcb = pcb; return ERR_OK; };

    REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
    REQUIRE(send_cmd(c, "RETR file.bin\r\n").empty()); /* awaiting the connect */
    mock_tcp_connect_fn = nullptr;

    int idx = mock_tcp_pcb_index(s_pcb);
    REQUIRE(idx >= 0);
    REQUIRE(mock_tcp_cb_err[idx] != nullptr);

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_err[idx](mock_tcp_cb_arg[idx], ERR_RST);
    REQUIRE(written_since(before) == "425 Can't open data connection.\r\n");

    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("a data connection accepted before the command still transfers", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 5; };
    g_retr_text = "abcde";
    read_once(nullptr, nullptr, nullptr, 0); /* reset */
    mock_lfs_file_read_fn = read_once;

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);

    /* Client races ahead and connects before issuing RETR. */
    accept_pasv_data_connection();

    std::string out = send_cmd(c, "RETR file.bin\r\n");
    REQUIRE(out.rfind("150 Opening BINARY", 0) == 0);
    REQUIRE(out.find("abcde") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("the data accept callback rejects a failed connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);

    struct tcp_pcb *pcb = tcp_new();
    int pasv_idx = mock_tcp_pcb_index(pcb) - 1;

    static struct tcp_pcb *s_aborted;
    s_aborted = nullptr;
    mock_tcp_abort_fn = [](struct tcp_pcb *p) { s_aborted = p; };

    REQUIRE(mock_tcp_cb_accept[pasv_idx](mock_tcp_cb_arg[pasv_idx], pcb, ERR_ABRT)
            == ERR_ABRT);
    REQUIRE(s_aborted == pcb);

    REQUIRE(mock_tcp_cb_accept[pasv_idx](mock_tcp_cb_arg[pasv_idx], nullptr, ERR_ABRT)
            == ERR_VAL);
    mock_tcp_abort_fn = nullptr;
}

TEST_CASE("RETR fails when the file cannot be opened", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_opencfg_fn = [](lfs_t *, lfs_file_t *, const char *, int,
                                  const struct lfs_file_config *) -> int { return -1; };
    REQUIRE(send_cmd(c, "RETR missing.txt\r\n") == "550 Failed to open file.\r\n");
}

TEST_CASE("a file read error aborts the transfer with 451", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 512; };
    mock_lfs_file_read_fn = [](lfs_t *, lfs_file_t *, void *, lfs_size_t) -> lfs_ssize_t {
        return -1; /* e.g. LFS_ERR_CORRUPT */
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR file.bin\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    /* Reporting 226 here would leave the client with a silently short file. */
    REQUIRE(out.find("226 Transfer complete.") == std::string::npos);
    REQUIRE(out.find("451 Requested action aborted") != std::string::npos);
}

TEST_CASE("RETR without an argument is rejected", "[transfer]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "RETR\r\n") == "501 Specify file name.\r\n");
}

TEST_CASE("a second transfer command while one is pending is refused", "[transfer]")
{
    /* Re-opening s->file / s->dir without closing corrupts littlefs's
     * intrusive list of open handles, so the second command must be
     * rejected outright rather than silently overwriting the handle. */
    init_server();
    Client c = connect_client();

    static int s_opens;
    s_opens = 0;
    mock_lfs_file_opencfg_fn = [](lfs_t *, lfs_file_t *, const char *, int,
                                  const struct lfs_file_config *) -> int {
        s_opens++;
        return 0;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR a.txt\r\n").empty()); /* awaiting data conn */

    SECTION("a second RETR") {
        REQUIRE(send_cmd(c, "RETR b.txt\r\n") == "450 Transfer already in progress.\r\n");
    }
    SECTION("a STOR") {
        REQUIRE(send_cmd(c, "STOR b.txt\r\n") == "450 Transfer already in progress.\r\n");
    }
    SECTION("a LIST") {
        REQUIRE(send_cmd(c, "LIST\r\n") == "450 Transfer already in progress.\r\n");
    }
    REQUIRE(s_opens == 1);

    /* PASV and ABOR both reset the state, so the client is never stuck. */
    REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");
    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR b.txt\r\n").empty());
    REQUIRE(s_opens == 2);
}

TEST_CASE("RETR over an active-mode PORT connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 5; };
    g_retr_text = "abcde";
    read_once(nullptr, nullptr, nullptr, 0); /* reset */
    mock_lfs_file_read_fn = read_once;

    static struct tcp_pcb  *s_connect_pcb;
    static tcp_connected_fn s_connect_cb;
    s_connect_pcb = nullptr;
    s_connect_cb  = nullptr;
    struct Hook {
        static err_t fn(struct tcp_pcb *pcb, const ip_addr_t *, u16_t, tcp_connected_fn connected)
        {
            s_connect_pcb = pcb;
            s_connect_cb  = connected;
            return ERR_OK;
        }
    };
    mock_tcp_connect_fn = Hook::fn;

    REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
    REQUIRE(send_cmd(c, "RETR file.bin\r\n").empty()); /* async: awaiting connect */
    mock_tcp_connect_fn = nullptr;

    REQUIRE(s_connect_pcb != nullptr);
    REQUIRE(s_connect_cb != nullptr);

    int idx = mock_tcp_pcb_index(s_connect_pcb);
    REQUIRE(idx >= 0);

    u16_t before = mock_tcp_write_len;
    err_t err = s_connect_cb(mock_tcp_cb_arg[idx], s_connect_pcb, ERR_OK);
    REQUIRE(err == ERR_OK);
    std::string out = written_since(before);

    REQUIRE(out.find("abcde") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("active-mode data connection failures are reported", "[transfer]")
{
    init_server();
    Client c = connect_client();

    SECTION("no PCB available for the outgoing connection") {
        REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
        mock_tcp_new_fn = []() -> struct tcp_pcb * { return nullptr; };
        REQUIRE(send_cmd(c, "RETR file.bin\r\n") == "425 Can't open data connection.\r\n");
        mock_tcp_new_fn = nullptr;
    }

    SECTION("tcp_connect refuses immediately") {
        static struct tcp_pcb *s_aborted;
        s_aborted = nullptr;
        mock_tcp_abort_fn = [](struct tcp_pcb *p) { s_aborted = p; };
        mock_tcp_connect_fn = [](struct tcp_pcb *, const ip_addr_t *, u16_t,
                                 tcp_connected_fn) -> err_t { return ERR_RTE; };

        REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
        REQUIRE(send_cmd(c, "RETR file.bin\r\n") == "425 Can't open data connection.\r\n");
        REQUIRE(s_aborted != nullptr);

        mock_tcp_connect_fn = nullptr;
        mock_tcp_abort_fn = nullptr;
    }

    SECTION("the connect completes with an error") {
        static tcp_connected_fn s_cb;
        static struct tcp_pcb  *s_pcb;
        s_cb = nullptr;
        s_pcb = nullptr;
        mock_tcp_connect_fn = [](struct tcp_pcb *pcb, const ip_addr_t *, u16_t,
                                 tcp_connected_fn connected) -> err_t {
            s_pcb = pcb;
            s_cb  = connected;
            return ERR_OK;
        };
        REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
        REQUIRE(send_cmd(c, "RETR file.bin\r\n").empty());
        mock_tcp_connect_fn = nullptr;

        int idx = mock_tcp_pcb_index(s_pcb);
        u16_t before = mock_tcp_write_len;
        /* lwIP has already freed the PCB by the time it reports the failure. */
        REQUIRE(s_cb(mock_tcp_cb_arg[idx], nullptr, ERR_ABRT) == ERR_OK);
        REQUIRE(written_since(before) == "425 Can't open data connection.\r\n");

        /* A detached connected callback must be harmless. */
        REQUIRE(s_cb(nullptr, nullptr, ERR_ABRT) == ERR_OK);
    }

    /* Every failure path must leave the session able to transfer again. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

/* ==================================================================== */
/*  STOR                                                                 */
/* ==================================================================== */

TEST_CASE("STOR receives uploaded data and closes cleanly", "[transfer]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());

    u16_t before = mock_tcp_write_len;
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    REQUIRE(written_since(before) == "150 Ok to send data.\r\n");

    int data_idx = mock_tcp_pcb_index(data_pcb);
    REQUIRE(mock_tcp_cb_recv[data_idx] != nullptr);

    std::string chunk = "payload-bytes";
    struct pbuf p = make_pbuf(chunk);
    before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p, ERR_OK);
    /* No reply yet — still receiving. */
    REQUIRE(mock_tcp_write_len == before);

    /* Client closes the data connection -> upload complete. */
    before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);
    REQUIRE(written_since(before) == "226 Transfer complete.\r\n");

    /* State is reset, so the next transfer is accepted. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("STOR writes every pbuf in a chain", "[transfer]")
{
    init_server();
    Client c = connect_client();

    static std::string s_written;
    s_written.clear();
    mock_lfs_file_write_fn = [](lfs_t *, lfs_file_t *, const void *buf,
                                lfs_size_t size) -> lfs_ssize_t {
        s_written.append(static_cast<const char *>(buf), size);
        return (lfs_ssize_t)size;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    std::string a = "first-", b = "second";
    struct pbuf p2 = make_pbuf(b);
    struct pbuf p1 = make_pbuf(a);
    p1.next    = &p2;
    p1.tot_len = (u16_t)(p1.len + p2.len);

    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p1, ERR_OK);
    REQUIRE(s_written == "first-second");
}

TEST_CASE("a detached data recv callback frees the pbuf and stops", "[transfer]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    static int s_frees;
    s_frees = 0;
    mock_pbuf_free_fn = [](struct pbuf *) -> u8_t { s_frees++; return 1; };

    std::string chunk = "x";
    struct pbuf p = make_pbuf(chunk);
    REQUIRE(mock_tcp_cb_recv[data_idx](nullptr, data_pcb, &p, ERR_OK) == ERR_OK);
    REQUIRE(s_frees == 1);
    REQUIRE(mock_tcp_cb_recv[data_idx](nullptr, data_pcb, nullptr, ERR_OK) == ERR_OK);
    REQUIRE(s_frees == 1);

    mock_pbuf_free_fn = nullptr;
}

TEST_CASE("STOR fails when the file cannot be created", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_opencfg_fn = [](lfs_t *, lfs_file_t *, const char *, int,
                                  const struct lfs_file_config *) -> int { return -1; };
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n") == "550 Failed to create file.\r\n");
}

TEST_CASE("STOR reports storage errors mid-transfer", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_write_fn = [](lfs_t *, lfs_file_t *, const void *, lfs_size_t) -> lfs_ssize_t {
        return -1;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());

    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    std::string chunk = "x";
    struct pbuf p = make_pbuf(chunk);
    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p, ERR_OK);
    REQUIRE(written_since(before) == "452 Insufficient storage space.\r\n");
}

TEST_CASE("STOR reports a failed final flush instead of 226", "[transfer]")
{
    /* Given an upload whose writes all succeed, but whose close fails —
     * littlefs buffers writes and only flushes the tail of the file from
     * lfs_file_close(), so a volume that fills up on the last chunk reports
     * the error there and nowhere else ... */
    init_server();
    Client c = connect_client();

    static int s_closes;
    s_closes = 0;
    mock_lfs_file_close_fn = [](lfs_t *, lfs_file_t *) -> int {
        s_closes++;
        return -1;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());

    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    std::string chunk = "payload-bytes";
    struct pbuf p = make_pbuf(chunk);
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p, ERR_OK);

    /* When the client closes the data connection to end the upload ... */
    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);

    /* Then the failure is reported: answering 226 would tell the client a
     * truncated file is safely stored. */
    REQUIRE(written_since(before) ==
            "451 Requested action aborted: local error in processing.\r\n");

    /* And the file is closed exactly once — ftp_close_data() must not close
     * a handle the flush check already released. */
    REQUIRE(s_closes == 1);

    /* State is still reset, so the session stays usable. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("STOR still reports 226 when the final flush succeeds", "[transfer]")
{
    /* The companion to the case above: the close hook is exercised, returns
     * success, and the client gets its 226. */
    init_server();
    Client c = connect_client();

    static int s_closes;
    s_closes = 0;
    mock_lfs_file_close_fn = [](lfs_t *, lfs_file_t *) -> int {
        s_closes++;
        return 0;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());

    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);

    REQUIRE(written_since(before) == "226 Transfer complete.\r\n");
    REQUIRE(s_closes == 1);
}

TEST_CASE("STOR without an argument is rejected", "[transfer]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "STOR\r\n") == "501 Specify file name.\r\n");
}

/* ==================================================================== */
/*  ABOR                                                                 */
/* ==================================================================== */

TEST_CASE("ABOR replies ok and tears down any data connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");

    /* A subsequent LIST needs PASV/PORT again — proves the data channel
     * state was reset. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("closing a session releases an open file and directory", "[transfer]")
{
    init_server();
    Client c = connect_client();

    static int s_file_closes, s_dir_closes;
    s_file_closes = 0;
    s_dir_closes  = 0;
    mock_lfs_file_close_fn = [](lfs_t *, lfs_file_t *) -> int { s_file_closes++; return 0; };
    mock_lfs_dir_close_fn  = [](lfs_t *, lfs_dir_t *) -> int  { s_dir_closes++;  return 0; };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR a.txt\r\n").empty());
    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
    REQUIRE(s_file_closes == 1);

    Client c2 = connect_client();
    REQUIRE(send_cmd(c2, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c2, "LIST\r\n").empty());
    REQUIRE(send_cmd(c2, "QUIT\r\n") == "221 Goodbye.\r\n");
    REQUIRE(s_dir_closes == 1);
}

/* ==================================================================== */
/*  Regression: in-flight active-mode connects                          */
/* ==================================================================== */

namespace {

/** tcp_connect hook that captures the PCB + callback instead of connecting. */
tcp_connected_fn  g_pending_cb;
struct tcp_pcb   *g_pending_pcb;

err_t capture_connect(struct tcp_pcb *pcb, const ip_addr_t *, u16_t,
                      tcp_connected_fn connected)
{
    g_pending_pcb = pcb;
    g_pending_cb  = connected;
    return ERR_OK;
}

/** Arm PORT + RETR and leave the outgoing connect in flight. */
void start_pending_connect(const Client &c)
{
    g_pending_cb  = nullptr;
    g_pending_pcb = nullptr;
    mock_tcp_connect_fn = capture_connect;
    REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
    REQUIRE(send_cmd(c, "RETR file.bin\r\n").empty());
    mock_tcp_connect_fn = nullptr;
    REQUIRE(g_pending_cb != nullptr);
    REQUIRE(g_pending_pcb != nullptr);
}

} // namespace

TEST_CASE("a half-open active-mode PCB is reclaimed when the session closes",
          "[transfer][port]")
{
    /* Given a session whose outgoing data connect is still in flight ... */
    init_server();
    Client c = connect_client();
    start_pending_connect(c);

    static struct tcp_pcb *s_watch;
    static bool            s_watch_closed;
    s_watch        = g_pending_pcb;
    s_watch_closed = false;
    mock_tcp_close_fn = [](struct tcp_pcb *pcb) -> err_t {
        if (pcb == s_watch) s_watch_closed = true;
        return ERR_OK;
    };

    /* When the client quits before the connect completes ... */
    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
    mock_tcp_close_fn = nullptr;

    /* Then the PCB was closed rather than leaked with callbacks still
     * pointing into the (now recycled) session. */
    REQUIRE(s_watch_closed);
}

TEST_CASE("a connect completing after teardown cannot hijack the session slot",
          "[transfer][port]")
{
    /* Given a pending connect whose session is closed and its slot reused ... */
    init_server();
    Client c = connect_client();
    start_pending_connect(c);

    int   pending_idx = mock_tcp_pcb_index(g_pending_pcb);
    void *stale_arg   = mock_tcp_cb_arg[pending_idx];
    REQUIRE(stale_arg != nullptr);

    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
    Client c2 = connect_client(); /* takes over the freed session slot */

    /* When the stale connect finally reports success ... */
    u16_t before = mock_tcp_write_len;
    err_t rc = g_pending_cb(stale_arg, g_pending_pcb, ERR_OK);

    /* Then it must be dropped silently: no 150/451 on the new client's
     * control connection, and no transfer started on its behalf. */
    REQUIRE(rc == ERR_OK);
    REQUIRE(written_since(before).empty());
    REQUIRE(send_cmd(c2, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
    REQUIRE(send_cmd(c2, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("ABOR during a pending connect discards the late connection",
          "[transfer][port]")
{
    init_server();
    Client c = connect_client();
    start_pending_connect(c);

    REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");

    /* The cancelled connect must not resurrect the aborted transfer. */
    u16_t before = mock_tcp_write_len;
    REQUIRE(g_pending_cb(mock_tcp_cb_arg[c.idx], g_pending_pcb, ERR_OK) == ERR_OK);
    REQUIRE(written_since(before).empty());
}

/* ==================================================================== */
/*  Regression: idle timeout vs. active transfers                       */
/* ==================================================================== */

TEST_CASE("data-channel progress keeps a long transfer from timing out",
          "[transfer][connect]")
{
    /* Given an upload whose control connection stays silent throughout ... */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    /* When it runs for well past the idle timeout, one segment per poll ... */
    std::string chunk = "payload";
    for (int i = 0; i < FTP_SERVER_IDLE_TIMEOUT_POLLS * 2; i++) {
        REQUIRE(mock_tcp_cb_poll[c.idx] != nullptr);
        mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
        /* A timed-out session has had its callbacks detached — checking here
         * pins the failure on the poll that killed the live transfer. */
        REQUIRE(mock_tcp_cb_recv[data_idx] != nullptr);
        struct pbuf p = make_pbuf(chunk);
        mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p, ERR_OK);
    }

    /* Then the session is still alive and the upload can finish. */
    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);
    REQUIRE(written_since(before) == "226 Transfer complete.\r\n");
    REQUIRE(send_cmd(c, "NOOP\r\n") == "200 NOOP ok.\r\n");
}

TEST_CASE("a stalled transfer still hits the idle timeout", "[transfer][connect]")
{
    /* The timeout must measure progress, not merely "a transfer is armed" —
     * otherwise a client that opens a data connection and goes quiet pins a
     * session slot forever. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    (void)accept_pasv_data_connection();

    for (int i = 0; i < FTP_SERVER_IDLE_TIMEOUT_POLLS; i++) {
        mock_tcp_cb_poll[c.idx](mock_tcp_cb_arg[c.idx], c.pcb);
    }
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
}

TEST_CASE("an active-mode upload clears the PORT target when it completes",
          "[transfer][port]")
{
    /* A finished STOR must reset the data channel exactly like every other
     * transfer, or the next command silently reuses the stale PORT target. */
    init_server();
    Client c = connect_client();
    start_pending_connect(c); /* PORT armed; RETR left pending */
    REQUIRE(send_cmd(c, "ABOR\r\n") == "226 ABOR command successful.\r\n");

    REQUIRE(send_cmd(c, "PORT 192,168,1,1,4,2\r\n") == "200 PORT command successful.\r\n");
    mock_tcp_connect_fn = capture_connect;
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    mock_tcp_connect_fn = nullptr;

    u16_t before = mock_tcp_write_len;
    REQUIRE(g_pending_cb(mock_tcp_cb_arg[c.idx], g_pending_pcb, ERR_OK) == ERR_OK);
    REQUIRE(written_since(before) == "150 Ok to send data.\r\n");

    int data_idx = mock_tcp_pcb_index(g_pending_pcb);
    before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], g_pending_pcb, nullptr, ERR_OK);
    REQUIRE(written_since(before) == "226 Transfer complete.\r\n");

    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

/* ==================================================================== */
/*  Regression: misc. error paths                                       */
/* ==================================================================== */

TEST_CASE("a short filesystem write aborts the upload", "[transfer]")
{
    /* Accepting a partial write would silently truncate the stored file. */
    init_server();
    Client c = connect_client();

    mock_lfs_file_write_fn = [](lfs_t *, lfs_file_t *, const void *,
                                lfs_size_t size) -> lfs_ssize_t {
        return (lfs_ssize_t)(size / 2); /* volume filled up mid-write */
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    std::string chunk = "payload";
    struct pbuf p = make_pbuf(chunk);
    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, &p, ERR_OK);
    REQUIRE(written_since(before) == "452 Insufficient storage space.\r\n");
}

TEST_CASE("PASV fails cleanly when the local address is unavailable", "[pasv]")
{
    /* Without the check the 227 reply would advertise an uninitialised
     * address and send the client's data connection nowhere. */
    init_server();
    Client c = connect_client();

    mock_tcp_tcp_get_tcp_addrinfo_fn = [](struct tcp_pcb *, int, ip_addr_t *,
                                          u16_t *) -> err_t { return ERR_VAL; };
    REQUIRE(send_cmd(c, "PASV\r\n") == "421 Cannot determine server address.\r\n");
    mock_tcp_tcp_get_tcp_addrinfo_fn = nullptr;

    /* The half-built listener was torn down with it. */
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

/* ==================================================================== */
/*  Regression: data-channel teardown and lwIP contract                 */
/* ==================================================================== */

namespace {

/* Set up a RETR that is stuck mid-file: the data socket takes only the first
 * five bytes of an eleven-byte file, so the transfer is still armed and the
 * data PCB still live when the test does something to it. */
struct StalledRetr {
    Client          ctrl;
    struct tcp_pcb *data_pcb;
    int             data_idx;
};

StalledRetr start_stalled_retr()
{
    StalledRetr r{};
    r.ctrl = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 11; };
    g_retr_text = "hello world";
    read_once(nullptr, nullptr, nullptr, 0); /* reset */
    mock_lfs_file_read_fn = read_once;

    REQUIRE(send_cmd(r.ctrl, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(r.ctrl, "RETR file.txt\r\n").empty());

    mock_tcp_track_sndbuf = 1;
    r.data_pcb = accept_pasv_data_connection(/*snd_buf=*/5);
    r.data_idx = mock_tcp_pcb_index(r.data_pcb);
    return r;
}

} // namespace

TEST_CASE("a download the client abandons is reported as aborted, not complete",
          "[transfer]")
{
    /* Given a RETR that has only delivered part of the file ... */
    init_server();
    StalledRetr r = start_stalled_retr();
    mock_tcp_track_sndbuf = 0;

    /* When the client closes the data connection instead of reading on ... */
    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[r.data_idx](mock_tcp_cb_arg[r.data_idx], r.data_pcb,
                                 nullptr, ERR_OK);

    /* Then it must not be told the (truncated) file arrived intact. */
    REQUIRE(written_since(before) == "426 Connection closed; transfer aborted.\r\n");

    /* And the session is reusable. */
    REQUIRE(send_cmd(r.ctrl, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("a failing close does not change how an abandoned download is reported",
          "[transfer]")
{
    /* Only STOR consults the close result — a download has nothing to flush,
     * so 426 must survive an lfs_file_close() that fails. */
    init_server();
    StalledRetr r = start_stalled_retr();
    mock_tcp_track_sndbuf  = 0;
    mock_lfs_file_close_fn = [](lfs_t *, lfs_file_t *) -> int { return -1; };

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[r.data_idx](mock_tcp_cb_arg[r.data_idx], r.data_pcb,
                                 nullptr, ERR_OK);

    REQUIRE(written_since(before) == "426 Connection closed; transfer aborted.\r\n");
}

TEST_CASE("an upload the client closes is still reported as complete",
          "[transfer]")
{
    /* The counterpart to the case above: for STOR the FIN *is* the end of
     * the transfer, so it must keep answering 226. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "STOR upload.bin\r\n").empty());
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);
    REQUIRE(written_since(before) == "226 Transfer complete.\r\n");
}

TEST_CASE("closing an idle data connection reports nothing", "[transfer]")
{
    /* No transfer was ever armed, so there is no transfer to report on —
     * neither a 226 nor a 426 belongs on the control channel here. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    struct tcp_pcb *data_pcb = accept_pasv_data_connection();
    int data_idx = mock_tcp_pcb_index(data_pcb);

    u16_t before = mock_tcp_write_len;
    mock_tcp_cb_recv[data_idx](mock_tcp_cb_arg[data_idx], data_pcb, nullptr, ERR_OK);
    REQUIRE(mock_tcp_write_len == before);
}

namespace {

struct tcp_pcb *g_dead_data_pcb;

/** tcp_write that fails permanently for one PCB and captures the rest. */
err_t write_fails_on_dead_pcb(struct tcp_pcb *pcb, const void *data,
                              u16_t len, u8_t)
{
    if (pcb == g_dead_data_pcb) return ERR_CONN;
    u16_t room = (u16_t)(MOCK_TCP_WRITE_CAPTURE_SIZE - mock_tcp_write_len);
    u16_t copy = (len < room) ? len : room;
    std::memcpy(mock_tcp_write_buf + mock_tcp_write_len, data, copy);
    mock_tcp_write_len = (u16_t)(mock_tcp_write_len + copy);
    return ERR_OK;
}

} // namespace

TEST_CASE("a permanently failing data write aborts instead of hanging",
          "[transfer]")
{
    /* Given a stalled RETR whose data socket has gone bad ... */
    init_server();
    StalledRetr r = start_stalled_retr();

    g_dead_data_pcb   = r.data_pcb;
    mock_tcp_write_fn = write_fails_on_dead_pcb;

    /* When the peer ACKs and lwIP asks for more data, but every write fails
     * with an error that will never clear (ERR_MEM would be back-pressure) ... */
    mock_tcp_ack(r.data_pcb, 64);
    u16_t before = mock_tcp_write_len;
    err_t err = mock_tcp_cb_sent[r.data_idx](mock_tcp_cb_arg[r.data_idx],
                                             r.data_pcb, 5);

    mock_tcp_write_fn     = nullptr;
    mock_tcp_track_sndbuf = 0;
    g_dead_data_pcb       = nullptr;

    /* Then the transfer is torn down at once rather than waiting for a
     * tcp_sent callback that will never come. */
    REQUIRE(err == ERR_OK);
    REQUIRE(written_since(before) == "426 Connection closed; transfer aborted.\r\n");
    REQUIRE(send_cmd(r.ctrl, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

TEST_CASE("a command pipelined behind an over-long line still runs",
          "[commands]")
{
    /* Given one segment holding a line that overflows the command buffer,
     * its terminator, and a second complete command ... */
    init_server();
    Client c = connect_client();

    std::string segment(FTP_SERVER_CMD_BUF_SIZE + 32, 'A');
    segment += "\r\nNOOP\r\n";

    /* When it is delivered in a single recv ... */
    std::string out = send_cmd(c, segment);

    /* Then the over-long line is rejected and the NOOP behind it survives:
     * truncating the segment would have thrown away the '\n' that ends the
     * bad line, swallowing the next command with it. */
    REQUIRE(out == "500 Command line too long.\r\n200 NOOP ok.\r\n");

    /* And the buffer is back in sync for whatever arrives next. */
    REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
}

TEST_CASE("a listener that fails to close is not aborted", "[lwip]")
{
    /* tcp_abort() asserts "don't call tcp_abort for listen-pcbs" in lwIP, so
     * the close-degrades-to-abort fallback must skip listening PCBs. */
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);

    static int s_aborts;
    s_aborts = 0;
    mock_tcp_close_fn = [](struct tcp_pcb *) -> err_t { return ERR_MEM; };
    mock_tcp_abort_fn = [](struct tcp_pcb *) { s_aborts++; };

    /* ABOR closes the data channel, listener included. */
    (void)send_cmd(c, "ABOR\r\n");

    mock_tcp_close_fn = nullptr;
    mock_tcp_abort_fn = nullptr;

    /* Only the connected data PCB may be aborted — never the listener. */
    REQUIRE(s_aborts == 0);
}
