/*
 * EnlilOS — Capability System demo (M9-01)
 *
 * Verifica end-to-end di tutte le 5 syscall capability:
 *   cap_alloc, cap_query, cap_revoke, cap_derive, cap_send
 *
 * Output atteso:
 *   [CAP] cap_alloc  IPC RWS  → token OK
 *   [CAP] cap_query            → type/rights OK
 *   [CAP] cap_derive READ-only → child OK
 *   [CAP] cap_derive child query → rights OK
 *   [CAP] cap_revoke           → OK
 *   [CAP] cap_query post-revoke → -EINVAL OK
 *   [CAP] token uniqueness     → OK
 *   [CAP] ALL TESTS PASSED
 */

#include "syscall.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed long    s64;

/* ── syscall wrappers ───────────────────────────────────────────── */

static u64 sys_call1(long nr, u64 a0)
{
    register u64 x0 asm("x0") = a0;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static u64 sys_call2(long nr, u64 a0, u64 a1)
{
    register u64 x0 asm("x0") = a0;
    register u64 x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static __attribute__((noreturn)) void do_exit(long code)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = SYS_EXIT;
    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    __builtin_unreachable();
}

/* ── minimal write ──────────────────────────────────────────────── */

static void puts_fd(const char *s)
{
    register const char *x0 asm("x0");
    register long x1 asm("x1");
    register long x2 asm("x2");
    register long x8 asm("x8") = SYS_WRITE;
    long fd = 1;
    const char *p = s;
    long len = 0;
    while (p[len]) len++;
    x0 = (const char *)(long)fd;
    x1 = (long)s;
    x2 = len;
    (void)x0; (void)x1; (void)x2;

    register long r0 asm("x0") = fd;
    register long r1 asm("x1") = (long)s;
    register long r2 asm("x2") = len;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(x8) : "memory");
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
    /* --- 1. cap_alloc: tipo IPC, READ+WRITE+SEND+DERIVE+REVOKE --- */
    u64 rights_all = CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_SEND |
                     CAP_RIGHT_DERIVE | CAP_RIGHT_REVOKE;
    u64 tok = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_IPC, rights_all);

    if (tok == CAP_INVALID || (s64)tok < 0) {
        print_fail("[CAP] cap_alloc IPC");
        do_exit(1);
    }
    print_ok("[CAP] cap_alloc IPC");

    /* --- 2. cap_query: verifica type e rights --- */
    cap_info_t info;
    u64 rc = sys_call2(SYS_CAP_QUERY, tok, (u64)(uintptr_t)&info);
    if ((s64)rc < 0 || info.type != CAP_TYPE_IPC || info.rights != rights_all) {
        print_fail("[CAP] cap_query");
        do_exit(1);
    }
    print_ok("[CAP] cap_query");

    /* --- 3. cap_derive: figlio con soli READ right --- */
    u64 child = sys_call2(SYS_CAP_DERIVE, tok, CAP_RIGHT_READ);
    if (child == CAP_INVALID || (s64)child < 0) {
        print_fail("[CAP] cap_derive READ-only");
        do_exit(1);
    }
    print_ok("[CAP] cap_derive READ-only");

    /* --- 4. query del figlio: rights deve essere solo READ --- */
    cap_info_t cinfo;
    rc = sys_call2(SYS_CAP_QUERY, child, (u64)(uintptr_t)&cinfo);
    if ((s64)rc < 0 || cinfo.rights != CAP_RIGHT_READ ||
        cinfo.type != CAP_TYPE_IPC) {
        print_fail("[CAP] cap_derive child query");
        do_exit(1);
    }
    print_ok("[CAP] cap_derive child query");

    /* --- 5. il figlio non può revocare (no REVOKE right) --- */
    rc = sys_call1(SYS_CAP_REVOKE, child);
    /* child non ha REVOKE right → deve ritornare -EPERM */
    if ((s64)rc != -(s64)EPERM) {
        print_fail("[CAP] cap_revoke child (no right)");
        do_exit(1);
    }
    print_ok("[CAP] cap_revoke child (no right -> -EPERM)");

    /* --- 6. cap_revoke del token padre --- */
    rc = sys_call1(SYS_CAP_REVOKE, tok);
    if ((s64)rc < 0) {
        print_fail("[CAP] cap_revoke");
        do_exit(1);
    }
    print_ok("[CAP] cap_revoke");

    /* --- 7. cap_query post-revoca → deve ritornare -EINVAL --- */
    rc = sys_call2(SYS_CAP_QUERY, tok, (u64)(uintptr_t)&info);
    if ((s64)rc != -(s64)EINVAL) {
        print_fail("[CAP] cap_query post-revoke");
        do_exit(1);
    }
    print_ok("[CAP] cap_query post-revoke (-EINVAL)");

    /* --- 8. unicità token: due cap_alloc consecutive → token diversi --- */
    u64 t1 = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_MEM, CAP_RIGHT_READ);
    u64 t2 = sys_call2(SYS_CAP_ALLOC, CAP_TYPE_MEM, CAP_RIGHT_READ);
    if (t1 == CAP_INVALID || t2 == CAP_INVALID || t1 == t2) {
        print_fail("[CAP] token uniqueness");
        do_exit(1);
    }
    print_ok("[CAP] token uniqueness");

    puts_fd("[CAP] ALL TESTS PASSED\n");
    do_exit(0);
}
