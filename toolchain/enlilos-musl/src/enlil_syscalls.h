#ifndef ENLILOS_BOOTSTRAP_SYSCALLS_H
#define ENLILOS_BOOTSTRAP_SYSCALLS_H

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_EXIT            3
#define SYS_CLOSE           5
#define SYS_MMAP            7
#define SYS_MUNMAP          8
#define SYS_EXECVE          10
#define SYS_FORK            11
#define SYS_WAITPID         12
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
#define SYS_IOCTL           54
#define SYS_UNAME           55

#define MAP_FAILED_VA ((unsigned long)(-1L))

#endif
