/*
 * EnlilOS Microkernel - Virtual File System (M5-02)
 *
 * Bootstrap VFS a mount table statica.
 * Oggi gira in-kernel per semplificare il bring-up, ma modella gia'
 * l'interfaccia a server filesystem separati.
 */

#ifndef ENLILOS_VFS_H
#define ENLILOS_VFS_H

#include "syscall.h"
#include "types.h"

#define VFS_MAX_MOUNTS   10U
#define VFS_NAME_MAX     32U

typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t mode;
} vfs_dirent_t;

struct vfs_mount;

typedef struct {
    const struct vfs_mount *mount;
    uint32_t                node_id;
    uint32_t                flags;
    uint64_t                pos;
    uint64_t                size_hint;
    uint32_t                dir_index;
    uintptr_t               cookie;
} vfs_file_t;

typedef struct {
    int     (*open)(const struct vfs_mount *mount, const char *relpath,
                    uint32_t flags, vfs_file_t *out);
    ssize_t (*read)(vfs_file_t *file, void *buf, size_t count);
    ssize_t (*write)(vfs_file_t *file, const void *buf, size_t count);
    int     (*readdir)(vfs_file_t *file, vfs_dirent_t *out);
    int     (*stat)(vfs_file_t *file, stat_t *out);
    int     (*close)(vfs_file_t *file);
    int     (*mkdir)(const struct vfs_mount *mount, const char *relpath,
                     uint32_t mode);
    int     (*unlink)(const struct vfs_mount *mount, const char *relpath);
    int     (*rename)(const struct vfs_mount *old_mount, const char *old_relpath,
                      const struct vfs_mount *new_mount, const char *new_relpath);
    int     (*fsync)(vfs_file_t *file);
    int     (*truncate)(const struct vfs_mount *mount, const char *relpath,
                        uint64_t size);
    int     (*sync)(const struct vfs_mount *mount);
} vfs_ops_t;

typedef struct vfs_mount {
    bool             active;
    bool             readonly;
    const char      *path;
    const char      *name;
    const char      *fs_type;
    const vfs_ops_t *ops;
    uintptr_t        ctx;
} vfs_mount_t;

void    vfs_init(void);
void    vfs_rescan(void);

int     vfs_open(const char *path, uint32_t flags, vfs_file_t *out);
ssize_t vfs_read(vfs_file_t *file, void *buf, size_t count);
ssize_t vfs_write(vfs_file_t *file, const void *buf, size_t count);
int     vfs_readdir(vfs_file_t *file, vfs_dirent_t *out);
int     vfs_stat(vfs_file_t *file, stat_t *out);
int     vfs_close(vfs_file_t *file);
int     vfs_mkdir(const char *path, uint32_t mode);
int     vfs_unlink(const char *path);
int     vfs_rename(const char *old_path, const char *new_path);
int     vfs_fsync(vfs_file_t *file);
int     vfs_truncate(const char *path, uint64_t size);
int     vfs_sync(void);

#endif /* ENLILOS_VFS_H */
