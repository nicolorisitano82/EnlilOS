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

#define VFSD_IO_BYTES   192U

typedef enum {
    VFSD_REQ_OPEN = 1,
    VFSD_REQ_READ = 2,
    VFSD_REQ_WRITE = 3,
    VFSD_REQ_READDIR = 4,
    VFSD_REQ_STAT = 5,
    VFSD_REQ_CLOSE = 6,
} vfsd_req_op_t;

typedef struct {
    uint32_t op;
    int32_t  handle;
    uint32_t flags;
    uint32_t count;
    uint64_t size;
    union {
        char    path[VFSD_IO_BYTES];
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
