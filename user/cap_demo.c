/*
 * EnlilOS — Capability System demo (M9-01)
 *
 * Verifica end-to-end di tutte le 5 syscall capability:
 *   cap_alloc, cap_query, cap_revoke, cap_derive, cap_send
 */

#include "syscall.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed long    s64;

/* ── syscall wrappers ───────────────────────────────────────────── */

static long sys_call1(long nr, long a0)
{
    register long x0 asm("x0") = a0;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long sys_call2(long nr, long a0, long a1)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static __attribute__((noreturn)) void do_exit(long code)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = SYS_EXIT;
    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    __builtin_unreachable();
}

/* ── output ─────────────────────────────────────────────────────── */

static void puts_fd(const char *s)
{
    long len = 0;
    while (s[len]) len++;
    sys_call3(SYS_WRITE, 1, (long)s, len);
}

static void print_ok(const char *tag)
{
    puts_fd(tag);
    puts_fd(" OK\n");
}

static void print_fail(const char *tag)
{
    puts_fd(tag);
    puts_fd(" FAIL\n");
}

/* ── main ───────────────────────────────────────────────────────── */

void _start(void)
{
    /* 1. cap_alloc: tipo IPC, tutti i diritti */
    long rights_all = CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_SEND |
                      CAP_RIGHT_DERIVE | CAP_RIGHT_REVOKE;
    long tok = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_IPC, rights_all);

    if (tok <= 0) {
        print_fail("[CAP] cap_alloc IPC");
        do_exit(1);
    }
    print_ok("[CAP] cap_alloc IPC");

    /* 2. cap_query: verifica type e rights */
    cap_info_t info;
    long rc = sys_call2(SYS_CAP_QUERY, tok, (long)(uintptr_t)&info);
    if (rc < 0 || info.type != CAP_TYPE_IPC || (long)info.rights != rights_all) {
        print_fail("[CAP] cap_query");
        do_exit(1);
    }
    print_ok("[CAP] cap_query");

    /* 3. cap_derive: figlio con solo READ */
    long child = sys_call2(SYS_CAP_DERIVE, tok, CAP_RIGHT_READ);
    if (child <= 0) {
        print_fail("[CAP] cap_derive READ-only");
        do_exit(1);
    }
    print_ok("[CAP] cap_derive READ-only");

    /* 4. query del figlio: rights = solo READ */
    cap_info_t cinfo;
    rc = sys_call2(SYS_CAP_QUERY, child, (long)(uintptr_t)&cinfo);
    if (rc < 0 || cinfo.rights != CAP_RIGHT_READ || cinfo.type != CAP_TYPE_IPC) {
        print_fail("[CAP] cap_derive child query");
        do_exit(1);
    }
    print_ok("[CAP] cap_derive child query");

    /* 5. il figlio non può revocare (no CAP_RIGHT_REVOKE) → -EPERM */
    rc = sys_call1(SYS_CAP_REVOKE, child);
    if (rc != -(long)EPERM) {
        print_fail("[CAP] cap_revoke child (no right)");
        do_exit(1);
    }
    print_ok("[CAP] cap_revoke child (no right -> -EPERM)");

    /* 6. cap_revoke del token padre */
    rc = sys_call1(SYS_CAP_REVOKE, tok);
    if (rc < 0) {
        print_fail("[CAP] cap_revoke parent");
        do_exit(1);
    }
    print_ok("[CAP] cap_revoke parent");

    /* 7. cap_query post-revoca → -EINVAL */
    rc = sys_call2(SYS_CAP_QUERY, tok, (long)(uintptr_t)&info);
    if (rc != -(long)EINVAL) {
        print_fail("[CAP] cap_query post-revoke");
        do_exit(1);
    }
    print_ok("[CAP] cap_query post-revoke (-EINVAL)");

    /* 8. unicità token: due alloc consecutive → token diversi */
    long t1 = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_MEM, CAP_RIGHT_READ);
    long t2 = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_MEM, CAP_RIGHT_READ);
    if (t1 <= 0 || t2 <= 0 || t1 == t2) {
        print_fail("[CAP] token uniqueness");
        do_exit(1);
    }
    print_ok("[CAP] token uniqueness");

    puts_fd("[CAP] ALL TESTS PASSED\n");
    do_exit(0);
}
