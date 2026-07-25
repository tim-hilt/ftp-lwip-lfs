/**
 * @file mock_lfs.c
 * @brief Mock implementations of the LittleFS functions used by ftp_server.c.
 */
#include "mock_lfs.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Hook pointers                                                     */
/* ------------------------------------------------------------------ */

int         (*mock_lfs_file_opencfg_fn)(lfs_t *, lfs_file_t *,
                 const char *, int,
                 const struct lfs_file_config *)           = NULL;
int         (*mock_lfs_file_close_fn)(lfs_t *, lfs_file_t *) = NULL;
lfs_ssize_t (*mock_lfs_file_read_fn)(lfs_t *, lfs_file_t *,
                 void *, lfs_size_t)                        = NULL;
lfs_ssize_t (*mock_lfs_file_write_fn)(lfs_t *, lfs_file_t *,
                 const void *, lfs_size_t)                  = NULL;
lfs_soff_t  (*mock_lfs_file_size_fn)(lfs_t *, lfs_file_t *) = NULL;

int (*mock_lfs_dir_open_fn)(lfs_t *, lfs_dir_t *, const char *)  = NULL;
int (*mock_lfs_dir_close_fn)(lfs_t *, lfs_dir_t *)               = NULL;
int (*mock_lfs_dir_read_fn)(lfs_t *, lfs_dir_t *, struct lfs_info *) = NULL;

int (*mock_lfs_stat_fn)(lfs_t *, const char *, struct lfs_info *)  = NULL;
int (*mock_lfs_remove_fn)(lfs_t *, const char *)                   = NULL;
int (*mock_lfs_mkdir_fn)(lfs_t *, const char *)                    = NULL;
int (*mock_lfs_rename_fn)(lfs_t *, const char *, const char *)     = NULL;

/* ------------------------------------------------------------------ */
/*  Convenience lfs_t                                                 */
/* ------------------------------------------------------------------ */

struct lfs_config mock_lfs_cfg;
lfs_t             mock_lfs;

/* ------------------------------------------------------------------ */
/*  Reset                                                             */
/* ------------------------------------------------------------------ */

void mock_lfs_reset(void)
{
    mock_lfs_file_opencfg_fn = NULL;
    mock_lfs_file_close_fn   = NULL;
    mock_lfs_file_read_fn    = NULL;
    mock_lfs_file_write_fn   = NULL;
    mock_lfs_file_size_fn    = NULL;
    mock_lfs_dir_open_fn     = NULL;
    mock_lfs_dir_close_fn    = NULL;
    mock_lfs_dir_read_fn     = NULL;
    mock_lfs_stat_fn         = NULL;
    mock_lfs_remove_fn       = NULL;
    mock_lfs_mkdir_fn        = NULL;
    mock_lfs_rename_fn       = NULL;

    memset(&mock_lfs_cfg, 0, sizeof(mock_lfs_cfg));
    mock_lfs_cfg.cache_size = 256; /* matches FTP_SERVER_FILE_CACHE_SIZE default */

    memset(&mock_lfs, 0, sizeof(mock_lfs));
    mock_lfs.cfg = &mock_lfs_cfg;
}

/* ------------------------------------------------------------------ */
/*  File operations                                                   */
/* ------------------------------------------------------------------ */

int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file, const char *path,
                     int flags, const struct lfs_file_config *config)
{
    if (mock_lfs_file_opencfg_fn)
        return mock_lfs_file_opencfg_fn(lfs, file, path, flags, config);
    (void)lfs; (void)file; (void)path; (void)flags; (void)config;
    return 0; /* success */
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file)
{
    if (mock_lfs_file_close_fn)
        return mock_lfs_file_close_fn(lfs, file);
    (void)lfs; (void)file;
    return 0;
}

lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
                           void *buffer, lfs_size_t size)
{
    if (mock_lfs_file_read_fn)
        return mock_lfs_file_read_fn(lfs, file, buffer, size);
    (void)lfs; (void)file; (void)buffer; (void)size;
    return 0; /* EOF */
}

lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
                            const void *buffer, lfs_size_t size)
{
    if (mock_lfs_file_write_fn)
        return mock_lfs_file_write_fn(lfs, file, buffer, size);
    (void)lfs; (void)file; (void)buffer;
    return (lfs_ssize_t)size; /* pretend we wrote everything */
}

lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file)
{
    if (mock_lfs_file_size_fn)
        return mock_lfs_file_size_fn(lfs, file);
    (void)lfs; (void)file;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Directory operations                                              */
/* ------------------------------------------------------------------ */

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path)
{
    if (mock_lfs_dir_open_fn)
        return mock_lfs_dir_open_fn(lfs, dir, path);
    (void)lfs; (void)dir; (void)path;
    return 0;
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir)
{
    if (mock_lfs_dir_close_fn)
        return mock_lfs_dir_close_fn(lfs, dir);
    (void)lfs; (void)dir;
    return 0;
}

int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info)
{
    if (mock_lfs_dir_read_fn)
        return mock_lfs_dir_read_fn(lfs, dir, info);
    (void)lfs; (void)dir; (void)info;
    return 0; /* end of directory */
}

/* ------------------------------------------------------------------ */
/*  Filesystem operations                                             */
/* ------------------------------------------------------------------ */

int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info)
{
    if (mock_lfs_stat_fn)
        return mock_lfs_stat_fn(lfs, path, info);
    (void)lfs; (void)path;
    /* Default: report a regular file of size 0. */
    if (info) {
        memset(info, 0, sizeof(*info));
        info->type = LFS_TYPE_REG;
    }
    return 0;
}

int lfs_remove(lfs_t *lfs, const char *path)
{
    if (mock_lfs_remove_fn)
        return mock_lfs_remove_fn(lfs, path);
    (void)lfs; (void)path;
    return 0;
}

int lfs_mkdir(lfs_t *lfs, const char *path)
{
    if (mock_lfs_mkdir_fn)
        return mock_lfs_mkdir_fn(lfs, path);
    (void)lfs; (void)path;
    return 0;
}

int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath)
{
    if (mock_lfs_rename_fn)
        return mock_lfs_rename_fn(lfs, oldpath, newpath);
    (void)lfs; (void)oldpath; (void)newpath;
    return 0;
}
