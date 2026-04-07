/*
 * EnlilOS Microkernel - Syscall Interface (M3-01 / M3-02)
 *
 * ABI AArch64:
 *   - Numero syscall in x8
 *   - Argomenti in x0–x5
 *   - Valore di ritorno in x0 (negativo = -errno)
 *
 * Tabella syscall_table[256] indicizzata direttamente: O(1), no ricerca.
 * Flag SYSCALL_FLAG_RT: syscall completano con priorità elevata.
 */

#ifndef ENLILOS_SYSCALL_H
#define ENLILOS_SYSCALL_H

#include "types.h"
#include "exception.h"
#include "signal.h"
#include "mreact.h"
#include "ksem.h"
#include "kmon.h"
#include "cap.h"

/* ── Numeri syscall ─────────────────────────────────────────────────── */

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_EXIT            3
#define SYS_OPEN            4
#define SYS_CLOSE           5
#define SYS_FSTAT           6
#define SYS_MMAP            7
#define SYS_MUNMAP          8
#define SYS_BRK             9
#define SYS_EXECVE          10
#define SYS_FORK            11
#define SYS_WAITPID         12
#define SYS_CLOCK_GETTIME   13
#define SYS_GETDENTS        14
#define SYS_TASK_SNAPSHOT   15
#define SYS_SPAWN           16
#define SYS_SIGACTION       17
#define SYS_SIGPROCMASK     18
#define SYS_SIGRETURN       19
#define SYS_YIELD           20
#define SYS_MREACT_SUBSCRIBE      80
#define SYS_MREACT_WAIT           81
#define SYS_MREACT_CANCEL         82
#define SYS_MREACT_SUBSCRIBE_ALL  83
#define SYS_MREACT_SUBSCRIBE_ANY  84
#define SYS_KSEM_CREATE           85
#define SYS_KSEM_OPEN             86
#define SYS_KSEM_CLOSE            87
#define SYS_KSEM_UNLINK           88
#define SYS_KSEM_POST             89
#define SYS_KSEM_WAIT             90
#define SYS_KSEM_TIMEDWAIT        91
#define SYS_KSEM_TRYWAIT          92
#define SYS_KSEM_GETVALUE         93
#define SYS_KSEM_ANON             94
#define SYS_KMON_CREATE           95
#define SYS_KMON_DESTROY          96
#define SYS_KMON_ENTER            97
#define SYS_KMON_EXIT             98
#define SYS_KMON_WAIT             99
#define SYS_KMON_SIGNAL           110
#define SYS_KMON_BROADCAST        111
#define SYS_KILL            134

/* Port / IPC server bootstrap (M9-02) */
#define SYS_PORT_LOOKUP     140
#define SYS_IPC_WAIT        141
#define SYS_IPC_REPLY       142

/* Bootstrap VFS backend per vfsd (M9-02 v1) */
#define SYS_VFS_BOOT_OPEN      150
#define SYS_VFS_BOOT_READ      151
#define SYS_VFS_BOOT_WRITE     152
#define SYS_VFS_BOOT_READDIR   153
#define SYS_VFS_BOOT_STAT      154
#define SYS_VFS_BOOT_CLOSE     155

/* Bootstrap Block backend per blkd (M9-03 v1) */
#define SYS_BLK_BOOT_READ      156
#define SYS_BLK_BOOT_WRITE     157
#define SYS_BLK_BOOT_FLUSH     158
#define SYS_BLK_BOOT_SECTORS   159

/* ── Capability System (M9-01): 60–64 ──────────────────────────────── */
#define SYS_CAP_ALLOC       60
#define SYS_CAP_SEND        61
#define SYS_CAP_REVOKE      62
#define SYS_CAP_DERIVE      63
#define SYS_CAP_QUERY       64

/* Range ANE (M3-03): 100–119 */
#define SYS_ANE_FIRST       100
#define SYS_ANE_LAST        119

#define SYSCALL_MAX         256

/* ── Codici di errore ───────────────────────────────────────────────── */

#define EPERM               1
#define ENOENT              2
#define ESRCH               3
#define ECHILD              10
#define EAGAIN              11
#define EINTR               4
#define ENOMEM              12
#define EFAULT              14
#define EBADF               9
#define EBUSY               16
#define EEXIST              17
#define EXDEV               18
#define ENOTDIR             20
#define EISDIR              21
#define EINVAL              22
#define ENFILE              23
#define ENOSPC              28
#define EROFS               30
#define ENAMETOOLONG        36
#define ENOTEMPTY           39
#define EIO                 5
#define ENOSYS              38
#define ETIMEDOUT           110

#define ERR(e)  ((uint64_t)(-(int64_t)(e)))

/* ── Flag open() ────────────────────────────────────────────────────── */

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     (1 << 6)
#define O_TRUNC     (1 << 9)
#define O_APPEND    (1 << 10)

/* ── Flag mmap() ────────────────────────────────────────────────────── */

#define PROT_NONE       0
#define PROT_READ       (1 << 0)
#define PROT_WRITE      (1 << 1)
#define PROT_EXEC       (1 << 2)

#define MAP_SHARED      (1 << 0)
#define MAP_PRIVATE     (1 << 1)
#define MAP_ANONYMOUS   (1 << 5)

/* Valore di errore per mmap (equivalente a (void*)-1) */
#define MAP_FAILED_VA   ((uint64_t)(uintptr_t)(-1LL))

/* ── Clock IDs per clock_gettime() ─────────────────────────────────── */

#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1

/* ── waitpid options ────────────────────────────────────────────────── */

#define WNOHANG     1

/* ── struct timespec ────────────────────────────────────────────────── */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_t;

/* ── struct stat minimale (M5-02) ──────────────────────────────────── */

#define S_IFMT      0170000U
#define S_IFREG     0100000U
#define S_IFDIR     0040000U
#define S_IFCHR     0020000U

#define S_IRUSR     0400U
#define S_IWUSR     0200U
#define S_IXUSR     0100U
#define S_IRGRP     0040U
#define S_IWGRP     0020U
#define S_IXGRP     0010U
#define S_IROTH     0004U
#define S_IWOTH     0002U
#define S_IXOTH     0001U

typedef struct {
    uint32_t st_mode;
    uint32_t st_blksize;
    uint64_t st_size;
    uint64_t st_blocks;
} stat_t;

/* ── struct dirent minimale ────────────────────────────────────────── */

typedef struct {
    char     name[32];
    uint32_t mode;
} sys_dirent_t;

/* ── snapshot task per top/monitoring ─────────────────────────────── */

#define TASK_SNAPSHOT_RUNNING   0U
#define TASK_SNAPSHOT_READY     1U
#define TASK_SNAPSHOT_BLOCKED   2U
#define TASK_SNAPSHOT_ZOMBIE    3U

typedef struct {
    uint32_t pid;
    uint8_t  priority;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  _reserved0;
    uint64_t runtime_ns;
    uint64_t budget_ns;
    uint64_t period_ms;
    uint64_t deadline_ms;
    char     name[32];
} task_snapshot_t;

/* ── Flag syscall ───────────────────────────────────────────────────── */

#define SYSCALL_FLAG_RT      (1 << 0)
#define SYSCALL_FLAG_NOBLOCK (1 << 1)

/* ── Handler e descrittore ──────────────────────────────────────────── */

typedef uint64_t (*syscall_handler_fn)(uint64_t args[6]);

typedef struct {
    syscall_handler_fn  handler;
    uint32_t            flags;
    const char         *name;
} syscall_entry_t;

/* ── API ────────────────────────────────────────────────────────────── */

void syscall_init(void);
void syscall_dispatch(exception_frame_t *frame);

extern syscall_entry_t syscall_table[SYSCALL_MAX];

#endif /* ENLILOS_SYSCALL_H */
