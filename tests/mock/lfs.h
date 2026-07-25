/**
 * @file lfs.h
 * @brief Mock LittleFS types and function declarations for unit testing.
 *
 * Only the subset used by ftp_server.c is defined.  Struct internals that
 * ftp_server.c never touches are replaced with opaque padding so that
 * sizeof(lfs_file_t) etc. remain nonzero but the real LFS headers are not
 * needed.
 */
#ifndef LFS_H
#define LFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Scalar typedefs                                                   */
/* ------------------------------------------------------------------ */

typedef uint32_t lfs_size_t;
typedef uint32_t lfs_off_t;
typedef int32_t  lfs_ssize_t;
typedef int32_t  lfs_soff_t;
typedef uint32_t lfs_block_t;

/* ------------------------------------------------------------------ */
/*  Constants used by ftp_server.c                                    */
/* ------------------------------------------------------------------ */

#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 255
#endif

enum {
    LFS_TYPE_REG = 0x001,
    LFS_TYPE_DIR = 0x002,
};

enum {
    LFS_O_RDONLY = 1,
    LFS_O_WRONLY = 2,
    LFS_O_RDWR   = 3,
    LFS_O_CREAT  = 0x0100,
    LFS_O_EXCL   = 0x0200,
    LFS_O_TRUNC  = 0x0400,
    LFS_O_APPEND = 0x0800,
};

/* ------------------------------------------------------------------ */
/*  Structs                                                           */
/* ------------------------------------------------------------------ */

/** File info returned by lfs_stat / lfs_dir_read. */
struct lfs_info {
    uint8_t    type;
    lfs_size_t size;
    char       name[LFS_NAME_MAX + 1];
};

/** Filesystem configuration — ftp_server.c only reads cfg->cache_size. */
struct lfs_config {
    void *context;
    int (*read)(const struct lfs_config *c, lfs_block_t block,
                lfs_off_t off, void *buffer, lfs_size_t size);
    int (*prog)(const struct lfs_config *c, lfs_block_t block,
                lfs_off_t off, const void *buffer, lfs_size_t size);
    int (*erase)(const struct lfs_config *c, lfs_block_t block);
    int (*sync)(const struct lfs_config *c);

    lfs_size_t read_size;
    lfs_size_t prog_size;
    lfs_size_t block_size;
    lfs_size_t block_count;
    int32_t    block_cycles;
    lfs_size_t cache_size;
    lfs_size_t lookahead_size;
    lfs_size_t compact_thresh;
    void      *read_buffer;
    void      *prog_buffer;
    void      *lookahead_buffer;
    lfs_size_t name_max;
    lfs_size_t file_max;
    lfs_size_t attr_max;
    lfs_size_t metadata_max;
    lfs_size_t inline_max;
};

/** Optional per-file config (used by lfs_file_opencfg). */
struct lfs_file_config {
    void              *buffer;
    struct lfs_attr   *attrs;
    lfs_size_t         attr_count;
};

/** Custom attribute (referenced by lfs_file_config). */
struct lfs_attr {
    uint8_t    type;
    void      *buffer;
    lfs_size_t size;
};

/**
 * Opaque directory handle.
 * ftp_server.c only passes pointers to lfs_dir_open/close/read — the
 * test mock stores its state elsewhere, so the struct is just padding.
 */
typedef struct lfs_dir {
    uint8_t _opaque[64];
} lfs_dir_t;

/**
 * Opaque file handle.
 * Same rationale as lfs_dir_t.
 */
typedef struct lfs_file {
    uint8_t _opaque[128];
} lfs_file_t;

/**
 * Filesystem handle.
 * ftp_server.c accesses only lfs->cfg->cache_size in ftp_server_init().
 */
typedef struct lfs {
    const struct lfs_config *cfg;
    /* Remaining real fields are unused by ftp_server.c. */
    uint8_t _opaque[128];
} lfs_t;

/* ------------------------------------------------------------------ */
/*  Function declarations (implemented in mock_lfs.c)                 */
/* ------------------------------------------------------------------ */

/* File operations */
int        lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file, const char *path,
                            int flags, const struct lfs_file_config *config);
int        lfs_file_close(lfs_t *lfs, lfs_file_t *file);
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
                           void *buffer, lfs_size_t size);
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
                            const void *buffer, lfs_size_t size);
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file);

/* Directory operations */
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);

/* Filesystem operations */
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);
int lfs_remove(lfs_t *lfs, const char *path);
int lfs_mkdir(lfs_t *lfs, const char *path);
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);

#ifdef __cplusplus
}
#endif

#endif /* LFS_H */
