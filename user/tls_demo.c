/*
 * EnlilOS — TLS / TPIDR_EL0 demo (M11-01b)
 *
 * Verifica che TPIDR_EL0 sia preservato:
 *   1. dopo sched_yield()         (preemption cooperativa)
 *   2. dopo fork(), nel parent    (TPIDR invariato)
 *   3. nel child di fork()        (TPIDR ereditato dal parent)
 *
 * exit(0) = tutto ok, exit(1..3) = fallimento passo corrispondente.
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;

static void say(const char *s)
{
    const char *p = s;
    while (*p) p++;
    user_svc3(SYS_WRITE, 1, (long)s, (long)(p - s));
}

static u64 tp_read(void)
{
    u64 v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static void tp_write(u64 v)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(v) : "memory");
}

static void print_hex(u64 v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[18];
    buf[0]  = '0'; buf[1]  = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[17] = '\n';
    user_svc3(SYS_WRITE, 1, (long)buf, 18);
}

int main(void)
{
    const u64 MAGIC = 0xDEADBEEF12345678ULL;

    /* ── Test 1: preservazione attraverso sched_yield ─────────────── */
    tp_write(MAGIC);
    user_svc0(SYS_YIELD);
    if (tp_read() != MAGIC) {
        say("[tls-tp] FAIL: TPIDR_EL0 corrotto dopo yield\n");
        user_svc_exit(1, SYS_EXIT);
    }

    /* ── Test 2: fork — parent ────────────────────────────────────── */
    long pid = user_svc0(SYS_FORK);
    if (pid < 0) {
        say("[tls-tp] FAIL: fork() fallito\n");
        user_svc_exit(2, SYS_EXIT);
    }

    if (pid == 0) {
        /* Child: verifica che abbia ereditato il TP del parent */
        if (tp_read() != MAGIC) {
            say("[tls-tp] FAIL child: TPIDR_EL0 non ereditato\n");
            user_svc_exit(3, SYS_EXIT);
        }
        say("[tls-tp] child: TPIDR_EL0 ereditato correttamente\n");
        user_svc_exit(0, SYS_EXIT);
    }

    /* Parent: verifica che il proprio TP non sia cambiato */
    if (tp_read() != MAGIC) {
        say("[tls-tp] FAIL parent: TPIDR_EL0 corrotto dopo fork\n");
        user_svc_exit(2, SYS_EXIT);
    }

    /* Parent: aspetta il child */
    long status = 0;
    user_svc3(SYS_WAITPID, pid, (long)&status, 0);
    int child_exit = (int)((status >> 8) & 0xFF);
    if (child_exit != 0) {
        say("[tls-tp] FAIL: child exit non-zero\n");
        user_svc_exit(child_exit, SYS_EXIT);
    }

    say("[tls-tp] PASS: TPIDR_EL0 corretto dopo yield e fork\n");
    user_svc_exit(0, SYS_EXIT);
}
