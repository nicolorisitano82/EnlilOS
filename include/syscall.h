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

/* Range ANE (M3-03): 100–119 */
#define SYS_ANE_FIRST       100
#define SYS_ANE_LAST        119

#define SYSCALL_MAX         256

/* ── Codici di errore ───────────────────────────────────────────────── */

#define EPERM               1
#define ENOENT              2
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
