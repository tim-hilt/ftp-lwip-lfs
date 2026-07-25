/**
 * @file ftp_test_support.hpp
 * @brief Shared harness for driving ftp_server.c through the lwIP mock.
 *
 * ftp_server.c only exposes ftp_server_init()/ftp_server_deinit() — all
 * protocol behaviour is exercised indirectly by driving the callbacks that
 * ftp_server.c registers with the mock lwIP layer (tcp_arg/tcp_recv/...),
 * exactly as real lwIP would invoke them:
 *
 *   1. ftp_server_init() registers ftp_ctrl_accept as the listener's accept
 *      callback -> captured in mock_tcp_cb_accept[kListenIdx] (the listener
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
 * No dynamic allocation beyond std::string is used, matching the project's
 * no-heap policy for the library itself.
 */
#ifndef FTP_TEST_SUPPORT_HPP
#define FTP_TEST_SUPPORT_HPP

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

extern "C" {
#include "ftp_server.h"
#include "mock_lwip.h"
#include "mock_lfs.h"
}

namespace ftptest {

/* The listener PCB is always the very first PCB tcp_new() hands out right
 * after mock_lwip_reset(), because ftp_server_init() is the first thing
 * every test does and it makes exactly one tcp_new() call. */
constexpr int kListenIdx = 0;

/** Return value of the most recent callback driven by a helper below. */
inline err_t g_last_err = ERR_OK;

struct Client {
    struct tcp_pcb *pcb;
    int             idx;
};

/** Reset both mocks and bring up the server against mock_lfs. */
inline void init_server()
{
    mock_lwip_reset();
    mock_lfs_reset();
    REQUIRE(ftp_server_init(&mock_lfs) == ERR_OK);
}

/** Simulate a new control connection arriving and return its handle. */
inline Client connect_client()
{
    struct tcp_pcb *pcb = tcp_new();
    REQUIRE(pcb != nullptr);
    REQUIRE(mock_tcp_cb_accept[kListenIdx] != nullptr);
    g_last_err = mock_tcp_cb_accept[kListenIdx](mock_tcp_cb_arg[kListenIdx], pcb, ERR_OK);
    REQUIRE(g_last_err == ERR_OK);
    Client c;
    c.pcb = pcb;
    c.idx = mock_tcp_pcb_index(pcb);
    REQUIRE(c.idx >= 0);
    return c;
}

/** Build a single-segment pbuf around `data` (must outlive the call). */
inline struct pbuf make_pbuf(const std::string &data)
{
    struct pbuf p{};
    p.next    = nullptr;
    p.payload = const_cast<char *>(data.data());
    p.len     = static_cast<u16_t>(data.size());
    p.tot_len = p.len;
    p.ref     = 1;
    return p;
}

/** Everything written since `since`. */
inline std::string written_since(u16_t since)
{
    return std::string(mock_tcp_write_buf + since,
                       static_cast<size_t>(mock_tcp_write_len - since));
}

/**
 * Feed one command line to a client's control connection and return exactly
 * the bytes ftp_server.c wrote out in response. The callback's return value
 * is left in g_last_err for tests that care about the lwIP contract.
 */
inline std::string send_cmd(const Client &c, const std::string &line)
{
    u16_t before = mock_tcp_write_len;
    struct pbuf p = make_pbuf(line);
    REQUIRE(mock_tcp_cb_recv[c.idx] != nullptr);
    g_last_err = mock_tcp_cb_recv[c.idx](mock_tcp_cb_arg[c.idx], c.pcb, &p, ERR_OK);
    return written_since(before);
}

/**
 * Simulate a client connecting to the most recently opened PASV listener.
 * Relies on the invariant that nothing else calls tcp_new() between the
 * PASV command and this call, so the new data PCB's pool index is exactly
 * one past the listener's.
 *
 * @param snd_buf Initial send window for the data PCB; shrink it (together
 *                with mock_tcp_track_sndbuf) to force back-pressure.
 */
inline struct tcp_pcb *accept_pasv_data_connection(u16_t snd_buf = 4096)
{
    struct tcp_pcb *data_pcb = tcp_new();
    REQUIRE(data_pcb != nullptr);
    data_pcb->snd_buf = snd_buf;
    int data_idx = mock_tcp_pcb_index(data_pcb);
    int pasv_idx = data_idx - 1;
    REQUIRE(mock_tcp_cb_accept[pasv_idx] != nullptr);
    g_last_err = mock_tcp_cb_accept[pasv_idx](mock_tcp_cb_arg[pasv_idx], data_pcb, ERR_OK);
    return data_pcb;
}

/** lfs_stat hook reporting every path as a directory. */
inline int stat_is_dir(lfs_t *, const char *, struct lfs_info *info)
{
    if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_DIR; }
    return 0;
}

/** lfs_stat hook reporting every path as a regular file. */
inline int stat_is_reg(lfs_t *, const char *, struct lfs_info *info)
{
    if (info) { std::memset(info, 0, sizeof(*info)); info->type = LFS_TYPE_REG; }
    return 0;
}

/** lfs_stat hook reporting every path as missing. */
inline int stat_missing(lfs_t *, const char *, struct lfs_info *)
{
    return -1;
}

} // namespace ftptest

#endif /* FTP_TEST_SUPPORT_HPP */
