#ifndef ENLILOS_MUSL_SYS_WAIT_H
#define ENLILOS_MUSL_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG     1
#define WUNTRACED   2

#define WIFEXITED(status)   ((((status) & 0x7F) == 0))
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) ((((status) & 0x7F) != 0) && (((status) & 0x7F) != 0x7F))
#define WTERMSIG(status)    ((status) & 0x7F)
#define WIFSTOPPED(status)  ((((status) & 0xFF) == 0x7F))
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)

pid_t waitpid(pid_t pid, int *status, int options);

#endif
