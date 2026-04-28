#ifndef ENLILOS_MUSL_SYS_IPC_H
#define ENLILOS_MUSL_SYS_IPC_H

#include <sys/types.h>
#include <stdint.h>

typedef int key_t;

#define IPC_PRIVATE  ((key_t)0)
#define IPC_CREAT    0001000
#define IPC_EXCL     0002000
#define IPC_NOWAIT   0004000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2
#define IPC_INFO     3

struct ipc_perm {
    key_t  __key;
    uid_t  uid;
    gid_t  gid;
    uid_t  cuid;
    gid_t  cgid;
    mode_t mode;
    int    __seq;
};

#endif /* ENLILOS_MUSL_SYS_IPC_H */
