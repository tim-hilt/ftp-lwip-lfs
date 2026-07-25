/**
 * @file test_main.cpp
 * @brief Catch2 unit tests for the FTP server, driven against the mock
 *        lwIP + LittleFS backends.
 *
 * Test strategy
 * -------------
 * ftp_server.c only exposes ftp_server_init()/ftp_server_deinit() — all
 * protocol behaviour is exercised indirectly by driving the callbacks that
 * ftp_server.c registers with the mock lwIP layer (tcp_arg/tcp_recv/...),
 * exactly as real lwIP would invoke them:
 *
 *   1. ftp_server_init() registers ftp_ctrl_accept as the listener's
 *      accept callback -> captured in mock_tcp_cb_accept[0] (the listener
 *      is always the first PCB allocated after mock_lwip_reset()).
 *   2. Simulating an incoming client = allocate a PCB via tcp_new() and
 *      invoke that captured accept callback with it.
 *   3. Simulating a client sending a command = build a struct pbuf around
 *      the command text and invoke the captured recv callback.
 *   4. Everything ftp_server.c writes (control replies *and* data-channel
 *      bytes) lands in the single shared mock_tcp_write_buf, so tests
 *      capture mock_tcp_write_len before/after an action and inspect the
 *      slice that was appended.
 *
 * No dynamic allocation is used anywhere in this file, matching the
 * project's no-heap policy for the library itself (tests are host-side
 * only, but there's no need to allocate regardless).
 */
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

extern "C" {
#include "ftp_server.h"
#include "mock_lwip.h"
#include "mock_lfs.h"
}

namespace {

/* The listener PCB is always the very first PCB tcp_new() hands out right
 * after mock_lwip_reset(), because ftp_server_init() is the first thing
 * every test does and it makes exactly one tcp_new() call. */
constexpr int kListenIdx = 0;

struct Client {
    struct tcp_pcb *pcb;
    int             idx;
};

/** Reset both mocks and bring up the server against mock_lfs. */
void init_server()
{
    mock_lwip_reset();
    mock_lfs_reset();
    REQUIRE(ftp_server_init(&mock_lfs) == ERR_OK);
}

/** Simulate a new control connection arriving and return its handle. */
Client connect_client()
{
    struct tcp_pcb *pcb = tcp_new();
    REQUIRE(pcb != nullptr);
    REQUIRE(mock_tcp_cb_accept[kListenIdx] != nullptr);
    err_t err = mock_tcp_cb_accept[kListenIdx](mock_tcp_cb_arg[kListenIdx], pcb, ERR_OK);
    REQUIRE(err == ERR_OK);
    Client c;
    c.pcb = pcb;
    c.idx = mock_tcp_pcb_index(pcb);
    REQUIRE(c.idx >= 0);
    return c;
}

/** Build a single-segment pbuf around `data` (must outlive the call). */
struct pbuf make_pbuf(const std::string &data)
{
    struct pbuf p{};
    p.next    = nullptr;
    p.payload = const_cast<char *>(data.data());
    p.len     = static_cast<u16_t>(data.size());
    p.tot_len = p.len;
    p.ref     = 1;
    return p;
}

/** Feed one command line to a client's control connection and return
 *  exactly the bytes ftp_server.c wrote out in response. */
std::string send_cmd(const Client &c, const std::string &line)
{
    u16_t before = mock_tcp_write_len;
    struct pbuf p = make_pbuf(line);
    REQUIRE(mock_tcp_cb_recv[c.idx] != nullptr);
    err_t err = mock_tcp_cb_recv[c.idx](mock_tcp_cb_arg[c.idx], c.pcb, &p, ERR_OK);
    REQUIRE(err == ERR_OK);
    return std::string(mock_tcp_write_buf + before,
                        static_cast<size_t>(mock_tcp_write_len - before));
}

/** Everything written since `since`. */
std::string written_since(u16_t since)
{
    return std::string(mock_tcp_write_buf + since,
                        static_cast<size_t>(mock_tcp_write_len - since));
}

/**
 * Simulate a client connecting to the most recently opened PASV listener.
 * Relies on the invariant that nothing else calls tcp_new() between the
 * PASV command and this call, so the new data PCB's pool index is exactly
 * one past the listener's.
 */
struct tcp_pcb *accept_pasv_data_connection()
{
    struct tcp_pcb *data_pcb = tcp_new();
    REQUIRE(data_pcb != nullptr);
    int data_idx = mock_tcp_pcb_index(data_pcb);
    int pasv_idx = data_idx - 1;
    REQUIRE(mock_tcp_cb_accept[pasv_idx] != nullptr);
    err_t err = mock_tcp_cb_accept[pasv_idx](mock_tcp_cb_arg[pasv_idx], data_pcb, ERR_OK);
    REQUIRE(err == ERR_OK);
    return data_pcb;
}

} // namespace

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

    int calls = 0;
    struct Counter {
        static struct tcp_pcb *fn(void) { return nullptr; }
    };
    mock_tcp_new_fn = Counter::fn;
    REQUIRE(ftp_server_init(&mock_lfs) == ERR_ARG);
    /* tcp_new() must never be reached — the cache check happens first. */
    mock_tcp_new_fn = nullptr;
    (void)calls;
}

TEST_CASE("ftp_server_init succeeds and registers the accept callback", "[init]")
{
    init_server();
    REQUIRE(mock_tcp_cb_accept[kListenIdx] != nullptr);
}

TEST_CASE("ftp_server_deinit tears down without crashing and is idempotent", "[init]")
{
    init_server();
    Client c = connect_client();
    (void)c;
    ftp_server_deinit();
    ftp_server_deinit(); /* must be safe to call again */
    SUCCEED();
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

    struct tcp_pcb *aborted_pcb = nullptr;
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
    aborted_pcb = pcb;
    (void)aborted_pcb;
}

/* ==================================================================== */
/*  Simple post-auth commands (no auth configured -> logged in on accept)*/
/* ==================================================================== */

TEST_CASE("basic informational commands", "[commands]")
{
    init_server();
    Client c = connect_client();

    SECTION("SYST") {
        REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
    }
    SECTION("FEAT") {
        REQUIRE(send_cmd(c, "FEAT\r\n") ==
                "211-Features:\r\n"
                " PASV\r\n"
                " SIZE\r\n"
                " UTF8\r\n"
                "211 End\r\n");
    }
    SECTION("NOOP") {
        REQUIRE(send_cmd(c, "NOOP\r\n") == "200 NOOP ok.\r\n");
    }
    SECTION("OPTS UTF8 ON") {
        REQUIRE(send_cmd(c, "OPTS UTF8 ON\r\n") == "200 UTF8 set to on.\r\n");
    }
    SECTION("OPTS unsupported option") {
        REQUIRE(send_cmd(c, "OPTS BOGUS\r\n") == "501 Option not understood.\r\n");
    }
    SECTION("unknown command") {
        REQUIRE(send_cmd(c, "WOOP\r\n") == "502 Command not implemented.\r\n");
    }
    SECTION("commands are case-insensitive") {
        REQUIRE(send_cmd(c, "syst\r\n") == "215 UNIX Type: L8\r\n");
    }
}

TEST_CASE("TYPE only accepts binary mode", "[commands]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "TYPE I\r\n") == "200 Switching to Binary mode.\r\n");
    REQUIRE(send_cmd(c, "TYPE A\r\n") ==
            "504 ASCII transfer mode not supported, use binary.\r\n");
    REQUIRE(send_cmd(c, "TYPE\r\n") ==
            "504 ASCII transfer mode not supported, use binary.\r\n");
}

TEST_CASE("QUIT replies and tears down the session", "[commands]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");

    /* The session is torn down: ftp_close_session() deregisters every
     * callback on the control PCB, proving no further processing can
     * happen through it. */
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
    REQUIRE(mock_tcp_cb_arg[c.idx] == nullptr);
}

TEST_CASE("multiple commands in one TCP segment are all processed", "[commands]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "NOOP\r\nNOOP\r\n") ==
            "200 NOOP ok.\r\n200 NOOP ok.\r\n");
}

TEST_CASE("an unterminated line exceeding the buffer is discarded", "[commands]")
{
    init_server();
    Client c = connect_client();

    std::string huge(FTP_SERVER_CMD_BUF_SIZE + 32, 'x'); /* no CR/LF at all */
    REQUIRE(send_cmd(c, huge) == "500 Command line too long.\r\n");
}

/* ==================================================================== */
/*  USER / PASS (server configured with no auth requirement)            */
/* ==================================================================== */

TEST_CASE("USER logs in immediately when no auth is configured", "[auth]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "USER anybody\r\n") == "230 Login successful.\r\n");
}

TEST_CASE("PASS without USER first is rejected", "[auth]")
{
    init_server();
    Client c = connect_client();
    /* Already logged in via accept (no auth configured), so PASS is not
     * awaited -> 503. */
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

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
        if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_DIR; }
        return 0;
    };

    REQUIRE(send_cmd(c, "CWD sub\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/sub\" is the current directory.\r\n");

    REQUIRE(send_cmd(c, "CDUP\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("CWD to a non-directory or missing path fails", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *) -> int {
        return -1; /* ENOENT */
    };
    REQUIRE(send_cmd(c, "CWD missing\r\n") == "550 Failed to change directory.\r\n");
    /* cwd is unchanged. */
    mock_lfs_stat_fn = nullptr;
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("relative path resolution collapses . and ..", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
        if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_DIR; }
        return 0;
    };

    REQUIRE(send_cmd(c, "CWD a/./b/../c\r\n") == "250 Directory successfully changed.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/a/c\" is the current directory.\r\n");
}

TEST_CASE("a resolved path that overflows FTP_SERVER_PATH_MAX is rejected", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
        if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_DIR; }
        return 0;
    };

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

/* ==================================================================== */
/*  DELE / MKD / RMD / RNFR / RNTO / SIZE                                */
/* ==================================================================== */

TEST_CASE("MKD success and failure", "[fs]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "MKD newdir\r\n") == "257 \"/newdir\" created.\r\n");

    mock_lfs_mkdir_fn = [](lfs_t *, const char *) -> int { return -1; };
    REQUIRE(send_cmd(c, "MKD newdir\r\n") == "550 Create directory failed.\r\n");
}

TEST_CASE("MKD without an argument is rejected", "[fs]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "MKD\r\n") == "501 Specify directory name.\r\n");
}

TEST_CASE("RMD success and failure", "[fs]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "RMD olddir\r\n") == "250 Remove directory successful.\r\n");

    mock_lfs_remove_fn = [](lfs_t *, const char *) -> int { return -1; };
    REQUIRE(send_cmd(c, "RMD olddir\r\n") == "550 Remove directory failed.\r\n");
}

TEST_CASE("DELE success and failure", "[fs]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "DELE file.txt\r\n") == "250 Delete operation successful.\r\n");

    mock_lfs_remove_fn = [](lfs_t *, const char *) -> int { return -1; };
    REQUIRE(send_cmd(c, "DELE file.txt\r\n") == "550 Delete operation failed.\r\n");
}

TEST_CASE("RNFR / RNTO rename flow", "[fs]")
{
    init_server();
    Client c = connect_client();

    SECTION("RNTO without RNFR is rejected") {
        REQUIRE(send_cmd(c, "RNTO b.txt\r\n") == "503 RNFR required first.\r\n");
    }

    SECTION("RNFR on a missing file fails") {
        mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *) -> int { return -1; };
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
}

TEST_CASE("SIZE reports file size or fails for non-files", "[fs]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
        if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_REG; info->size = 12345; }
        return 0;
    };
    REQUIRE(send_cmd(c, "SIZE file.bin\r\n") == "213 12345\r\n");

    mock_lfs_stat_fn = [](lfs_t *, const char *, struct lfs_info *info) -> int {
        if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_DIR; }
        return 0;
    };
    REQUIRE(send_cmd(c, "SIZE adir\r\n") == "550 Could not get file size.\r\n");
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

TEST_CASE("PORT parses a valid h1,h2,h3,h4,p1,p2 argument", "[port]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "PORT 10,0,0,1,4,1\r\n") == "200 PORT command successful.\r\n");
}

TEST_CASE("PORT rejects malformed arguments", "[port]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "PORT garbage\r\n") == "501 Syntax error in parameters.\r\n");
    REQUIRE(send_cmd(c, "PORT 10,0,0,1,4\r\n") == "501 Syntax error in parameters.\r\n");
    REQUIRE(send_cmd(c, "PORT 300,0,0,1,4,1\r\n") == "501 Syntax error in parameters.\r\n");
    REQUIRE(send_cmd(c, "PORT\r\n") == "501 Syntax error in parameters.\r\n");
}

TEST_CASE("LIST/RETR before PASV or PORT fails", "[transfer]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "LIST\r\n") == "425 Use PASV or PORT first.\r\n");
}

/* ==================================================================== */
/*  LIST / NLST                                                         */
/* ==================================================================== */

TEST_CASE("LIST streams directory entries and completes", "[transfer]")
{
    init_server();
    Client c = connect_client();

    static int call = 0;
    call = 0;
    mock_lfs_dir_read_fn = [](lfs_t *, lfs_dir_t *, struct lfs_info *info) -> int {
        if (call == 0) {
            info->type = LFS_TYPE_REG;
            info->size = 42;
            std::strcpy(info->name, "foo.txt");
            call++;
            return 1;
        }
        if (call == 1) {
            info->type = LFS_TYPE_DIR;
            info->size = 0;
            std::strcpy(info->name, "sub");
            call++;
            return 1;
        }
        return 0;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "LIST\r\n").empty()); /* async: waits for data conn */

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    REQUIRE(out.rfind("150 Here comes the data.\r\n", 0) == 0);
    REQUIRE(out.find("foo.txt") != std::string::npos);
    REQUIRE(out.find("sub") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("NLST lists bare names", "[transfer]")
{
    init_server();
    Client c = connect_client();

    static int call = 0;
    call = 0;
    mock_lfs_dir_read_fn = [](lfs_t *, lfs_dir_t *, struct lfs_info *info) -> int {
        if (call == 0) {
            info->type = LFS_TYPE_REG;
            std::strcpy(info->name, "one.txt");
            call++;
            return 1;
        }
        return 0;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "NLST\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);
    REQUIRE(out.find("one.txt\r\n") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

/* ==================================================================== */
/*  RETR                                                                 */
/* ==================================================================== */

TEST_CASE("RETR sends file content over the data connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 11; };

    static int call = 0;
    call = 0;
    mock_lfs_file_read_fn = [](lfs_t *, lfs_file_t *, void *buffer, lfs_size_t) -> lfs_ssize_t {
        if (call == 0) {
            const char *data = "hello world";
            std::memcpy(buffer, data, std::strlen(data));
            call++;
            return (lfs_ssize_t)std::strlen(data);
        }
        return 0;
    };

    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227", 0) == 0);
    REQUIRE(send_cmd(c, "RETR file.txt\r\n").empty());

    u16_t before = mock_tcp_write_len;
    accept_pasv_data_connection();
    std::string out = written_since(before);

    REQUIRE(out.rfind("150 Opening ASCII mode data connection for file.txt (11 bytes).\r\n", 0) == 0);
    REQUIRE(out.find("hello world") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);
}

TEST_CASE("RETR fails when the file cannot be opened", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_opencfg_fn = [](lfs_t *, lfs_file_t *, const char *, int,
                                  const struct lfs_file_config *) -> int { return -1; };
    REQUIRE(send_cmd(c, "RETR missing.txt\r\n") == "550 Failed to open file.\r\n");
}

TEST_CASE("RETR without an argument is rejected", "[transfer]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "RETR\r\n") == "501 Specify file name.\r\n");
}

TEST_CASE("RETR over an active-mode PORT connection", "[transfer]")
{
    init_server();
    Client c = connect_client();

    mock_lfs_file_size_fn = [](lfs_t *, lfs_file_t *) -> lfs_soff_t { return 5; };
    static int call = 0;
    call = 0;
    mock_lfs_file_read_fn = [](lfs_t *, lfs_file_t *, void *buffer, lfs_size_t) -> lfs_ssize_t {
        if (call == 0) {
            std::memcpy(buffer, "abcde", 5);
            call++;
            return 5;
        }
        return 0;
    };

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

    REQUIRE(send_cmd(c, "PORT 10,0,0,2,4,2\r\n") == "200 PORT command successful.\r\n");
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
