/*
 * EnlilOS musl bootstrap — sys/resource.h implementation
 * Wraps SYS_PRLIMIT64 (212).
 */

#include <sys/resource.h>
#include <errno.h>
#include "enlil_syscalls.h"
#include "../include/user_svc.h"

/* prlimit64: args pid, resource, new_limit*, old_limit* */
int prlimit64(pid_t pid, int resource,
              const struct rlimit64 *new_limit,
              struct rlimit64 *old_limit)
{
    long rc = user_svc4(SYS_PRLIMIT64,
                        (long)pid,
                        (long)resource,
                        (long)new_limit,
                        (long)old_limit);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }
    return 0;
}

int getrlimit64(int resource, struct rlimit64 *rlim)
{
    return prlimit64(0, resource, (const struct rlimit64 *)0, rlim);
}

int setrlimit64(int resource, const struct rlimit64 *rlim)
{
    return prlimit64(0, resource, rlim, (struct rlimit64 *)0);
}

int prlimit(pid_t pid, int resource,
            const struct rlimit *new_limit,
            struct rlimit *old_limit)
{
    struct rlimit64 new64, old64;
    int rc;

    if (new_limit) {
        new64.rlim_cur = (rlim64_t)new_limit->rlim_cur;
        new64.rlim_max = (rlim64_t)new_limit->rlim_max;
    }
    rc = prlimit64(pid, resource,
                   new_limit ? &new64 : (const struct rlimit64 *)0,
                   old_limit ? &old64  : (struct rlimit64 *)0);
    if (rc == 0 && old_limit) {
        old_limit->rlim_cur = (rlim_t)old64.rlim_cur;
        old_limit->rlim_max = (rlim_t)old64.rlim_max;
    }
    return rc;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    return prlimit(0, resource, (const struct rlimit *)0, rlim);
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    return prlimit(0, resource, rlim, (struct rlimit *)0);
}
