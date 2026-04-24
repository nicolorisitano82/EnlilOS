/*
 * linux_interp_demo.c — M11-05d: ELF with Linux-compat PT_INTERP
 *
 * Same content as dynamic_demo but linked with
 * --dynamic-linker=/lib/ld-linux-aarch64.so.1.
 * The ELF loader must fall back to /LD-ENLIL.SO when that path is absent.
 */
#include "user_svc.h"

typedef unsigned long u64;

extern const char *dyn_msg(void);
extern u64 dyn_len(void);

static long sys_write(long fd, const void *buf, u64 len)
{
    return user_svc3(1, fd, (long)buf, (long)len);
}

static void sys_exit(long code)
{
    user_svc_exit(code, 3);
}

void _start(void)
{
    const char *msg = dyn_msg();
    u64         len = dyn_len();

    (void)sys_write(1, msg, len);
    sys_exit(0);
}
