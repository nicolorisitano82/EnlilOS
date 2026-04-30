#ifndef ENLILOS_BOOTSTRAP_SYSCALLS_H
#define ENLILOS_BOOTSTRAP_SYSCALLS_H

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_EXIT            3
#define SYS_FSTAT           6
#define SYS_CLOSE           5
#define SYS_MMAP            7
#define SYS_MUNMAP          8
#define SYS_EXECVE          10
#define SYS_FORK            11
#define SYS_WAITPID         12
#define SYS_GETDENTS        14
#define SYS_SIGACTION       17
#define SYS_SIGPROCMASK     18
#define SYS_YIELD           20
#define SYS_SETPGID         21
#define SYS_GETPGID         22
#define SYS_SETSID          23
#define SYS_GETSID          25
#define SYS_TCSETPGRP       26
#define SYS_TCGETPGRP       27
#define SYS_CHDIR           28
#define SYS_GETCWD          29
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
#define SYS_DLOPEN          67
#define SYS_DLSYM           68
#define SYS_DLCLOSE         69
#define SYS_DLERROR         70
#define SYS_FUTEX           65
#define SYS_TGKILL          66
#define SYS_KSEM_CREATE     85
#define SYS_KSEM_OPEN       86
#define SYS_KSEM_CLOSE      87
#define SYS_KSEM_UNLINK     88
#define SYS_KSEM_POST       89
#define SYS_KSEM_WAIT       90
#define SYS_KSEM_TIMEDWAIT  91
#define SYS_KSEM_TRYWAIT    92
#define SYS_KSEM_GETVALUE   93
#define SYS_KSEM_ANON       94
#define SYS_KILL            134

/* Resource limits (prlimit64) */
#define SYS_PRLIMIT64       212

/* Reboot/poweroff */
#define SYS_REBOOT          213

/* epoll v1 */
#define SYS_EPOLL_CREATE1   214
#define SYS_EPOLL_CTL       215
#define SYS_EPOLL_PWAIT     216

/* BSD socket API (M10-03) */
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

/* SysV IPC (M11-05c) */
#define SYS_SHMGET          217
#define SYS_SHMAT           218
#define SYS_SHMDT           219
#define SYS_SHMCTL          220
#define SYS_SEMGET          221
#define SYS_SEMOP           222
#define SYS_SEMCTL          223

/* Wayland compositor (M12-01) */
#define SYS_WLD_PRESENT     224

#define MAP_FAILED_VA ((unsigned long)(-1L))
#define SYS_READLINKAT      227

#endif
