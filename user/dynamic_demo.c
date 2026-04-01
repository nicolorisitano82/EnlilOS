typedef unsigned long u64;

extern const char *dyn_msg(void);
extern u64 dyn_len(void);

static long sys_write(long fd, const void *buf, u64 len)
{
    register long       x0 asm("x0") = fd;
    register const void *x1 asm("x1") = buf;
    register u64        x2 asm("x2") = len;
    register long       x8 asm("x8") = 1;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x8)
                 : "memory");
    return x0;
}

static void sys_exit(long code)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = 3;

    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    for (;;)
        asm volatile("wfe");
}

void _start(void)
{
    const char *msg = dyn_msg();
    u64         len = dyn_len();

    (void)sys_write(1, msg, len);
    sys_exit(0);
}
