#ifndef ENLILOS_USER_SVC_H
#define ENLILOS_USER_SVC_H

static inline long user_svc0(long nr)
{
    register long x0 asm("x0") = 0;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8)
                 : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc1(long nr, long a0)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = 0;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1)
                 : "r"(x8)
                 : "x2", "x3", "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc2(long nr, long a0, long a1)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1)
                 : "r"(x8)
                 : "x2", "x3", "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2)
                 : "r"(x8)
                 : "x3", "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc4(long nr, long a0, long a1, long a2, long a3)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                 : "r"(x8)
                 : "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc5(long nr, long a0, long a1, long a2, long a3, long a4)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x4 asm("x4") = a4;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
                 : "r"(x8)
                 : "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline long user_svc6(long nr,
                             long a0, long a1, long a2,
                             long a3, long a4, long a5)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x4 asm("x4") = a4;
    register long x5 asm("x5") = a5;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)
                 : "r"(x8)
                 : "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static inline __attribute__((noreturn)) void user_svc_exit(long code, long nr)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 :
                 : "r"(x0), "r"(x8)
                 : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    for (;;)
        asm volatile("wfe");
}

#endif
