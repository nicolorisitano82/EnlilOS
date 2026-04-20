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
#include "termios.h"

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
#define SYS_SETPGID         21
#define SYS_GETPGID         22
#define SYS_SETSID          23
#define SYS_MSYNC           24  /* mmap file-backed write-back (M8-02) */
#define SYS_GETSID          25
#define SYS_TCSETPGRP       26
#define SYS_TCGETPGRP       27
#define SYS_CHDIR           28
#define SYS_GETCWD          29
#define SYS_MOUNT           30
#define SYS_UMOUNT          31
#define SYS_PIVOT_ROOT      32
#define SYS_UNSHARE         33
#define SYS_PIPE            34
#define SYS_DUP             35
#define SYS_DUP2            36
#define SYS_TCGETATTR       37
#define SYS_TCSETATTR       38
#define SYS_GETPID          39
#define SYS_GETPPID         40
#define SYS_ISATTY          41
#define SYS_GETTIMEOFDAY    42
#define SYS_NANOSLEEP       43
#define SYS_GETUID          44
#define SYS_GETGID          45
#define SYS_GETEUID         46
#define SYS_GETEGID         47
#define SYS_LSEEK           48
#define SYS_READV           49
#define SYS_WRITEV          50
#define SYS_FCNTL           51
#define SYS_OPENAT          52
#define SYS_FSTATAT         53
#define SYS_NEWFSTATAT      SYS_FSTATAT
#define SYS_IOCTL           54
#define SYS_UNAME           55
#define SYS_CLONE           56
#define SYS_GETTID          57
#define SYS_SET_TID_ADDRESS 58
#define SYS_EXIT_GROUP      59
#define SYS_MKDIR           71
#define SYS_UNLINK          72
#define SYS_RENAME          73
#define SYS_KBD_SET_LAYOUT  74
#define SYS_KBD_GET_LAYOUT  75
#define SYS_DLOPEN          67
#define SYS_DLSYM           68
#define SYS_DLCLOSE         69
#define SYS_DLERROR         70
#define SYS_FUTEX           65
#define SYS_TGKILL          66
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
#define SYS_IPC_POLL        143

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
#define SYS_VFS_BOOT_TASKINFO  160
#define SYS_VFS_BOOT_LSEEK     161
#define SYS_NET_BOOT_SEND      162
#define SYS_NET_BOOT_RECV      163
#define SYS_NET_BOOT_INFO      164

/* ── BSD Socket API (M10-03): 200–211 ──────────────────────────── */
#define SYS_SOCKET          200
#define SYS_BIND            201
#define SYS_LISTEN          202
#define SYS_ACCEPT          203
#define SYS_CONNECT         204
#define SYS_SEND            205
#define SYS_RECV            206
#define SYS_SENDTO          207
#define SYS_RECVFROM        208
#define SYS_SETSOCKOPT      209
#define SYS_GETSOCKOPT      210
#define SYS_SHUTDOWN        211

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
#define ENODEV              19
#define ECHILD              10
#define EAGAIN              11
#define EACCES              13
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
#define EPIPE               32
#define ERANGE              34
#define ENAMETOOLONG        36
#define ELOOP               40
#define ENOTEMPTY           39
#define EIO                 5
#define ENOSYS              38
#define ENOTTY              25
#define ESPIPE              29
#define ETIMEDOUT           110
#define EINPROGRESS         115
#define EOPNOTSUPP          95
#define EAFNOSUPPORT        97
#define EADDRINUSE          98
#define EADDRNOTAVAIL       99
#define ENETUNREACH         101
#define ECONNRESET          104
#define ENOBUFS             105
#define EISCONN             106
#define ENOTCONN            107
#define ECONNREFUSED        111
#define ENOTSOCK            88
#define ENOPROTOOPT         92

#define ERR(e)  ((uint64_t)(-(int64_t)(e)))

/* ── Flag open() ────────────────────────────────────────────────────── */

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     (1 << 6)
#define O_TRUNC     (1 << 9)
#define O_APPEND    (1 << 10)
#define O_NONBLOCK  (1 << 11)
#define O_CLOEXEC   (1 << 12)

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

/* Flags msync */
#define MS_ASYNC        1
#define MS_SYNC         4
#define MS_INVALIDATE   2

/* ── Flag mount/unshare bootstrap (M9-04) ─────────────────────────── */

#define MS_RDONLY       1U
#define MS_BIND         4096U

#define CLONE_NEWNS     0x00020000U
#define CLONE_VM        0x00000100U
#define CLONE_FS        0x00000200U
#define CLONE_FILES     0x00000400U
#define CLONE_SIGHAND   0x00000800U
#define CLONE_THREAD    0x00010000U
#define CLONE_SYSVSEM   0x00040000U
#define CLONE_SETTLS    0x00080000U
#define CLONE_PARENT_SETTID  0x00100000U
#define CLONE_CHILD_CLEARTID 0x00200000U
#define CLONE_CHILD_SETTID   0x01000000U

/* ── futex ─────────────────────────────────────────────────────── */

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CMD_MASK      127

/* ── lseek / openat / fcntl / ioctl ─────────────────────────────── */

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

#define AT_FDCWD    (-100)
#define AT_SYMLINK_NOFOLLOW  0x0100
#define AT_EMPTY_PATH        0x1000

#define FD_CLOEXEC          1

#define F_DUPFD             0
#define F_GETFD             1
#define F_SETFD             2
#define F_GETFL             3
#define F_SETFL             4
#define F_DUPFD_CLOEXEC     1030

#define TCGETS              0x5401UL
#define TCSETS              0x5402UL
#define TIOCGPGRP           0x540FUL
#define TIOCSPGRP           0x5410UL
#define TIOCGWINSZ          0x5413UL
#define FIONBIO             0x5421UL

/* ── Clock IDs per clock_gettime() ─────────────────────────────────── */

#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1

/* ── waitpid options ────────────────────────────────────────────────── */

#define WNOHANG     1
#define WUNTRACED   2

#define WIFEXITED(status)   ((((status) & 0x7F) == 0))
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSTOPPED(status)  ((((status) & 0xFF) == 0x7F))
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)

/* ── struct timespec ────────────────────────────────────────────────── */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} timeval_t;

typedef int64_t off_t;
typedef uint32_t socklen_t;

typedef struct {
    void   *iov_base;
    size_t  iov_len;
} iovec_t;

#define IOV_MAX     16

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

#define UTSNAME_FIELD_LEN  65
typedef struct {
    char sysname[UTSNAME_FIELD_LEN];
    char nodename[UTSNAME_FIELD_LEN];
    char release[UTSNAME_FIELD_LEN];
    char version[UTSNAME_FIELD_LEN];
    char machine[UTSNAME_FIELD_LEN];
    char domainname[UTSNAME_FIELD_LEN];
} utsname_t;

/* ── struct stat minimale (M5-02) ──────────────────────────────────── */

#define S_IFMT      0170000U
#define S_IFIFO     0010000U
#define S_IFREG     0100000U
#define S_IFDIR     0040000U
#define S_IFCHR     0020000U
#define S_IFLNK     0120000U
#define S_IFSOCK    0140000U

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
int  syscall_describe_fd_current(int fd, char *out, size_t cap);

extern syscall_entry_t syscall_table[SYSCALL_MAX];

#endif /* ENLILOS_SYSCALL_H */
