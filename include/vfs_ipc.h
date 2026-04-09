/*
 * EnlilOS - VFS IPC protocol (M9-02)
 *
 * Protocollo minimo tra i client kernel-side e il server user-space vfsd.
 * Il backend filesystem resta bootstrap in-kernel per M9-02 v1, ma il
 * namespace e il path open/read/write/readdir/stat/close passano via IPC.
 */

#ifndef ENLILOS_VFS_IPC_H
#define ENLILOS_VFS_IPC_H

#include "syscall.h"
#include "vfs.h"

#define VFSD_IO_BYTES       192U
#define VFSD_PATH_BYTES      96U

typedef enum {
    VFSD_FS_NONE = 0,
    VFSD_FS_BIND = 1,
    VFSD_FS_INITRD = 2,
    VFSD_FS_DEVFS = 3,
    VFSD_FS_PROCFS = 4,
    VFSD_FS_EXT4_DATA = 5,
    VFSD_FS_EXT4_SYSROOT = 6,
} vfsd_fs_type_t;

typedef enum {
    VFSD_REQ_OPEN = 1,
    VFSD_REQ_READ = 2,
    VFSD_REQ_WRITE = 3,
    VFSD_REQ_READDIR = 4,
    VFSD_REQ_STAT = 5,
    VFSD_REQ_CLOSE = 6,
    VFSD_REQ_RESOLVE = 7,
    VFSD_REQ_CHDIR = 8,
    VFSD_REQ_GETCWD = 9,
    VFSD_REQ_MOUNT = 10,
    VFSD_REQ_UMOUNT = 11,
    VFSD_REQ_UNSHARE = 12,
    VFSD_REQ_PIVOT_ROOT = 13,
    VFSD_REQ_LSEEK = 14,
} vfsd_req_op_t;

typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t sid;
} vfsd_taskinfo_t;

typedef struct {
    uint32_t op;
    int32_t  handle;
    uint32_t flags;
    uint32_t count;
    uint32_t arg0;
    uint32_t arg1;
    union {
        struct {
            char path[VFSD_PATH_BYTES];
            char aux[VFSD_PATH_BYTES];
        } paths;
        uint8_t data[VFSD_IO_BYTES];
    } u;
} vfsd_request_t;

typedef struct {
    int32_t  status;
    int32_t  handle;
    uint32_t data_len;
    uint32_t _pad0;
    union {
        stat_t       st;
        vfs_dirent_t dirent;
        uint8_t      data[VFSD_IO_BYTES];
    } u;
} vfsd_response_t;

#endif /* ENLILOS_VFS_IPC_H */
