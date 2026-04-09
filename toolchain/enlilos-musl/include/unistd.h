#ifndef ENLILOS_MUSL_UNISTD_H
#define ENLILOS_MUSL_UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <time.h>
#include <termios.h>

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

extern char **environ;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
pid_t   getpid(void);
pid_t   gettid(void);
pid_t   getppid(void);
uid_t   getuid(void);
gid_t   getgid(void);
uid_t   geteuid(void);
gid_t   getegid(void);
int     isatty(int fd);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     pipe(int fds[2]);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
pid_t   fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
void    _exit(int status) __attribute__((noreturn));

#endif
