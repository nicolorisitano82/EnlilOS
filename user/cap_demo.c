/*
 * EnlilOS — Capability System demo (M9-01)
 *
 * Verifica end-to-end delle 5 syscall capability.
 * Pattern output come fork_demo: nessun registro asm duplicato.
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed long    s64;

/* ── syscall wrappers — identici a fork_demo ────────────────────── */

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call2(long nr, long a0, long a1)
{
    return user_svc2(nr, a0, a1);
}

static long sys_call1(long nr, long a0)
{
    return user_svc1(nr, a0);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

/* ── write helper ─────────────────────────────────────────────────── */

static u64 cap_strlen(const char *s)
{
    u64 n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void cap_puts(const char *s)
{
    long len = (long)cap_strlen(s);
    (void)sys_call3(SYS_WRITE, 1, (long)s, len);
}

/* ── test helpers ─────────────────────────────────────────────────── */

static void ok(const char *tag)  { cap_puts(tag); cap_puts(" OK\n");   }
static void fail(const char *tag){ cap_puts(tag); cap_puts(" FAIL\n"); }

/* ── main ─────────────────────────────────────────────────────────── */

void _start(void)
{
    cap_puts("[CAP] boot\n");

    /* 1. cap_alloc */
    long rights = (long)(CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_SEND |
                         CAP_RIGHT_DERIVE | CAP_RIGHT_REVOKE);
    long tok = sys_call2((long)SYS_CAP_ALLOC, (long)CAP_TYPE_IPC, rights);

    cap_puts("[CAP] after alloc\n");

    if (tok <= 0) { fail("[CAP] 1 cap_alloc"); sys_exit_now(1); }
    ok("[CAP] 1 cap_alloc");

    /* 2. cap_query */
    cap_info_t info;
    long rc = sys_call2((long)SYS_CAP_QUERY, tok, (long)(uintptr_t)&info);
    if (rc < 0 || (long)info.type != (long)CAP_TYPE_IPC) {
        fail("[CAP] 2 cap_query"); sys_exit_now(1);
    }
    ok("[CAP] 2 cap_query");

    /* 3. cap_derive: solo READ */
    long child = sys_call2((long)SYS_CAP_DERIVE, tok, (long)CAP_RIGHT_READ);
    if (child <= 0) { fail("[CAP] 3 cap_derive"); sys_exit_now(1); }
    ok("[CAP] 3 cap_derive");

    /* 4. query del figlio */
    cap_info_t cinfo;
    rc = sys_call2((long)SYS_CAP_QUERY, child, (long)(uintptr_t)&cinfo);
    if (rc < 0 || (long)cinfo.rights != (long)CAP_RIGHT_READ) {
        fail("[CAP] 4 derive-query"); sys_exit_now(1);
    }
    ok("[CAP] 4 derive-query");

    /* 5. revoke figlio senza diritto → -EPERM */
    rc = sys_call1((long)SYS_CAP_REVOKE, child);
    if (rc != -(long)EPERM) { fail("[CAP] 5 revoke-noperm"); sys_exit_now(1); }
    ok("[CAP] 5 revoke-noperm");

    /* 6. revoke padre */
    rc = sys_call1((long)SYS_CAP_REVOKE, tok);
    if (rc < 0) { fail("[CAP] 6 revoke"); sys_exit_now(1); }
    ok("[CAP] 6 revoke");

    /* 7. query post-revoca → -EINVAL */
    rc = sys_call2((long)SYS_CAP_QUERY, tok, (long)(uintptr_t)&info);
    if (rc != -(long)EINVAL) { fail("[CAP] 7 post-revoke"); sys_exit_now(1); }
    ok("[CAP] 7 post-revoke");

    /* 8. unicità token */
    long t1 = sys_call2((long)SYS_CAP_ALLOC, (long)CAP_TYPE_MEM, (long)CAP_RIGHT_READ);
    long t2 = sys_call2((long)SYS_CAP_ALLOC, (long)CAP_TYPE_MEM, (long)CAP_RIGHT_READ);
    if (t1 <= 0 || t2 <= 0 || t1 == t2) { fail("[CAP] 8 uniqueness"); sys_exit_now(1); }
    ok("[CAP] 8 uniqueness");

    cap_puts("[CAP] ALL TESTS PASSED\n");
    sys_exit_now(0);
}
