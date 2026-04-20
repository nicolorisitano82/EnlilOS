/*
 * EnlilOS musl bootstrap — sys/reboot.h implementation
 * Wraps SYS_REBOOT (213).
 */

#include <sys/reboot.h>
#include <errno.h>
#include "enlil_syscalls.h"
#include "../include/user_svc.h"

int reboot(int cmd)
{
    long rc = user_svc1(SYS_REBOOT, (long)(unsigned int)cmd);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }
    return 0;
}
