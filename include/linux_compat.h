/*
 * EnlilOS - Linux AArch64 compatibility ABI (M11-05)
 *
 * Numeri e strutture minime usate dal dispatch Linux compat.
 */

#ifndef ENLILOS_LINUX_COMPAT_H
#define ENLILOS_LINUX_COMPAT_H

#include "types.h"

#define LINUX_SYSCALL_MAX      512U

/* Linux AArch64 syscall numbers (subset M11-05 v1) */
#define LINUX_NR_io_setup          0
#define LINUX_NR_getcwd            17
#define LINUX_NR_dup               23
#define LINUX_NR_dup3              24
#define LINUX_NR_fcntl             25
#define LINUX_NR_flock             32
#define LINUX_NR_ioctl             29
#define LINUX_NR_mkdirat           34
#define LINUX_NR_unlinkat          35
#define LINUX_NR_symlinkat         36
#define LINUX_NR_readlinkat        78
#define LINUX_NR_renameat          38
#define LINUX_NR_fchmod            52
#define LINUX_NR_fchmodat          53
#define LINUX_NR_fchownat          54
#define LINUX_NR_faccessat         48
#define LINUX_NR_faccessat2        439
#define LINUX_NR_openat            56
#define LINUX_NR_close             57
#define LINUX_NR_pipe2             59
#define LINUX_NR_getdents64        61
#define LINUX_NR_lseek             62
#define LINUX_NR_read              63
#define LINUX_NR_write             64
#define LINUX_NR_readv             65
#define LINUX_NR_writev            66
#define LINUX_NR_pselect6          72
#define LINUX_NR_ppoll             73
#define LINUX_NR_readlink          78 /* compat helper, non standard separate on arm64 */
#define LINUX_NR_newfstatat        79
#define LINUX_NR_fstat             80
#define LINUX_NR_fsync             82
#define LINUX_NR_truncate          45
#define LINUX_NR_ftruncate         46
#define LINUX_NR_exit              93
#define LINUX_NR_exit_group        94
#define LINUX_NR_wait4             260
#define LINUX_NR_set_tid_address   96
#define LINUX_NR_futex             98
#define LINUX_NR_nanosleep         101
#define LINUX_NR_clock_gettime     113
#define LINUX_NR_sched_getaffinity 123
#define LINUX_NR_sched_yield       124
#define LINUX_NR_sched_get_priority_max 143
#define LINUX_NR_sched_get_priority_min 144
#define LINUX_NR_tgkill            131
#define LINUX_NR_getrusage         165
#define LINUX_NR_umask             166
#define LINUX_NR_rt_sigaction      134
#define LINUX_NR_rt_sigprocmask    135
#define LINUX_NR_rt_sigreturn      139
#define LINUX_NR_uname             160
#define LINUX_NR_gettimeofday      169
#define LINUX_NR_getpid            172
#define LINUX_NR_getppid           173
#define LINUX_NR_getuid            174
#define LINUX_NR_geteuid           175
#define LINUX_NR_getgid            176
#define LINUX_NR_getegid           177
#define LINUX_NR_gettid            178
#define LINUX_NR_sysinfo           179
#define LINUX_NR_socket            198
#define LINUX_NR_bind              200
#define LINUX_NR_listen            201
#define LINUX_NR_accept            202
#define LINUX_NR_connect           203
#define LINUX_NR_getsockname       204
#define LINUX_NR_getpeername       205
#define LINUX_NR_sendto            206
#define LINUX_NR_recvfrom          207
#define LINUX_NR_setsockopt        208
#define LINUX_NR_getsockopt        209
#define LINUX_NR_shutdown          210
#define LINUX_NR_clone             220
#define LINUX_NR_execve            221
#define LINUX_NR_mmap              222
#define LINUX_NR_mprotect          226
#define LINUX_NR_munmap            215
#define LINUX_NR_brk               214
#define LINUX_NR_prlimit64         261
#define LINUX_NR_madvise           233
#define LINUX_NR_getrandom         278
#define LINUX_NR_rseq              293

/* Linux poll/select flags */
#define LINUX_POLLIN        0x0001U
#define LINUX_POLLPRI       0x0002U
#define LINUX_POLLOUT       0x0004U
#define LINUX_POLLERR       0x0008U
#define LINUX_POLLHUP       0x0010U
#define LINUX_POLLNVAL      0x0020U

/* Linux flock operations */
#define LINUX_LOCK_SH       1U
#define LINUX_LOCK_EX       2U
#define LINUX_LOCK_NB       4U
#define LINUX_LOCK_UN       8U

/* Linux AT_ flags */
#define LINUX_AT_FDCWD             (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW  0x0100
#define LINUX_AT_REMOVEDIR         0x0200
#define LINUX_AT_EMPTY_PATH        0x1000

/* Linux RLIMIT ids */
#define LINUX_RLIMIT_CPU           0
#define LINUX_RLIMIT_FSIZE         1
#define LINUX_RLIMIT_DATA          2
#define LINUX_RLIMIT_STACK         3
#define LINUX_RLIMIT_CORE          4
#define LINUX_RLIMIT_NOFILE        7
#define LINUX_RLIMIT_AS            9

typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} linux_rlimit64_t;

typedef struct {
    uintptr_t sa_handler;
    uint64_t  sa_flags;
    uintptr_t sa_restorer;
    uint64_t  sa_mask;
} linux_sigaction_t;

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime;
    uint64_t st_atime_nsec;
    int64_t  st_mtime;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime;
    uint64_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} linux_stat_t;

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

typedef struct {
    int64_t  ru_utime_sec;   /* Linux AArch64: long = 64-bit */
    int64_t  ru_utime_usec;
    int64_t  ru_stime_sec;
    int64_t  ru_stime_usec;
    int64_t  ru_maxrss;
    int64_t  ru_ixrss;
    int64_t  ru_idrss;
    int64_t  ru_isrss;
    int64_t  ru_minflt;
    int64_t  ru_majflt;
    int64_t  ru_nswap;
    int64_t  ru_inblock;
    int64_t  ru_oublock;
    int64_t  ru_msgsnd;
    int64_t  ru_msgrcv;
    int64_t  ru_nsignals;
    int64_t  ru_nvcsw;
    int64_t  ru_nivcsw;
} linux_rusage_t;

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} linux_dirent64_t;

typedef struct {
    int32_t  fd;
    int16_t  events;
    int16_t  revents;
} linux_pollfd_t;

typedef struct {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint8_t  _f[8];
} linux_sysinfo_t;

#endif /* ENLILOS_LINUX_COMPAT_H */
