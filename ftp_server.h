/**
 * @file ftp_server.h
 * @brief Minimal FTP server using LwIP raw TCP API and LittleFS.
 *
 * Usage:
 *   1. Mount your LittleFS filesystem.
 *   2. Call ftp_server_init() with the mounted lfs_t handle.
 *   3. The server listens on the configured port (default 21).
 *
 * Configuration macros (define before including or in your build):
 *   FTP_SERVER_PORT           - Control port (default 21)
 *   FTP_SERVER_PASV_PORT_MIN  - First passive data port (default 1024)
 *   FTP_SERVER_PASV_PORT_MAX  - Last  passive data port (default 1039)
 *   FTP_SERVER_MAX_CLIENTS    - Maximum concurrent sessions (default 2)
 *   FTP_SERVER_USER           - Required username, or NULL to skip auth
 *   FTP_SERVER_PASS           - Required password, or NULL to skip auth
 *   FTP_SERVER_DATA_BUF_SIZE  - Per-session transfer buffer (default 512).
 *                               One LIST entry is 47 + strlen(name) bytes, so
 *                               a buffer below 47 + LFS_NAME_MAX truncates
 *                               listing lines for the longest names.
 *   FTP_SERVER_CMD_BUF_SIZE   - Per-session command line buffer (default 256)
 *   FTP_SERVER_PATH_MAX       - Maximum path length (default 256)
 *   FTP_SERVER_FILE_CACHE_SIZE - LFS file cache per session (default 256,
 *                                must be >= lfs_config.cache_size)
 *   FTP_SERVER_IDLE_TIMEOUT_POLLS - Idle polls before disconnect (default 60)
 */

#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include "lfs.h"
#include "lwip/tcp.h"
#include "lwip/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Compile-time configuration with defaults                          */
/* ------------------------------------------------------------------ */

#ifndef FTP_SERVER_PORT
#define FTP_SERVER_PORT 21
#endif

#ifndef FTP_SERVER_PASV_PORT_MIN
#define FTP_SERVER_PASV_PORT_MIN 1024
#endif

#ifndef FTP_SERVER_PASV_PORT_MAX
#define FTP_SERVER_PASV_PORT_MAX 1039
#endif

#ifndef FTP_SERVER_MAX_CLIENTS
#define FTP_SERVER_MAX_CLIENTS 2
#endif

#ifndef FTP_SERVER_USER
#define FTP_SERVER_USER NULL
#endif

#ifndef FTP_SERVER_PASS
#define FTP_SERVER_PASS NULL
#endif

#ifndef FTP_SERVER_DATA_BUF_SIZE
#define FTP_SERVER_DATA_BUF_SIZE 512
#endif

#ifndef FTP_SERVER_CMD_BUF_SIZE
#define FTP_SERVER_CMD_BUF_SIZE 256
#endif

#ifndef FTP_SERVER_PATH_MAX
#define FTP_SERVER_PATH_MAX 256
#endif

#ifndef FTP_SERVER_FILE_CACHE_SIZE
/** Must be >= the cache_size used in your lfs_config; ftp_server_init()
 *  rejects a mismatch that would overflow this buffer. */
#define FTP_SERVER_FILE_CACHE_SIZE 256
#endif

#ifndef FTP_SERVER_IDLE_TIMEOUT_POLLS
/** Number of tcp_poll intervals (~5 s each) before closing an idle session. */
#define FTP_SERVER_IDLE_TIMEOUT_POLLS 60
#endif

/**
 * Initialise the FTP server.
 *
 * @param lfs  Pointer to an already-mounted LittleFS instance.
 * @return     ERR_OK on success, or an lwIP error code.
 */
err_t ftp_server_init(lfs_t *lfs);

/**
 * Stop the FTP server and close all client sessions.
 */
void ftp_server_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* FTP_SERVER_H */
