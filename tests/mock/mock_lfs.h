/**
 * @file mock_lfs.h
 * @brief Test-side control surface for the LittleFS mock.
 *
 * Same pattern as mock_lwip.h — per-function hooks + a reset call.
 */
#ifndef MOCK_LFS_H
#define MOCK_LFS_H

#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Per-function hooks (NULL → default)                               */
/* ------------------------------------------------------------------ */

extern int         (*mock_lfs_file_opencfg_fn)(lfs_t *, lfs_file_t *,
                        const char *, int,
                        const struct lfs_file_config *);
extern int         (*mock_lfs_file_close_fn)(lfs_t *, lfs_file_t *);
extern lfs_ssize_t (*mock_lfs_file_read_fn)(lfs_t *, lfs_file_t *,
                        void *, lfs_size_t);
extern lfs_ssize_t (*mock_lfs_file_write_fn)(lfs_t *, lfs_file_t *,
                        const void *, lfs_size_t);
extern lfs_soff_t  (*mock_lfs_file_size_fn)(lfs_t *, lfs_file_t *);

extern int (*mock_lfs_dir_open_fn)(lfs_t *, lfs_dir_t *, const char *);
extern int (*mock_lfs_dir_close_fn)(lfs_t *, lfs_dir_t *);
extern int (*mock_lfs_dir_read_fn)(lfs_t *, lfs_dir_t *, struct lfs_info *);

extern int (*mock_lfs_stat_fn)(lfs_t *, const char *, struct lfs_info *);
extern int (*mock_lfs_remove_fn)(lfs_t *, const char *);
extern int (*mock_lfs_mkdir_fn)(lfs_t *, const char *);
extern int (*mock_lfs_rename_fn)(lfs_t *, const char *, const char *);

/* ------------------------------------------------------------------ */
/*  Convenience: pre-built lfs_t + lfs_config for tests               */
/* ------------------------------------------------------------------ */

/**
 * A ready-to-use lfs_t whose cfg->cache_size is set to a safe value.
 * Pass &mock_lfs to ftp_server_init().
 */
extern lfs_t              mock_lfs;
extern struct lfs_config  mock_lfs_cfg;

/* ------------------------------------------------------------------ */
/*  Reset                                                             */
/* ------------------------------------------------------------------ */

/**
 * Reset all hooks and the convenience lfs_t/lfs_config.
 * Call at the start of every test.
 */
void mock_lfs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LFS_H */
