/*
 * glibc_compat.c — Glibc compatibility shim (M11-05g)
 *
 * Esporta simboli glibc che musl non espone o espone diversamente.
 * Compilato come shared object PIC senza libc (-nostdlib -fPIC).
 *
 * Simboli forniti:
 *   __libc_start_main       — entry point glibc-style
 *   gnu_get_libc_version    — ritorna "2.38" (versione fittizia)
 *   gnu_get_libc_release    — ritorna "stable"
 *   __cxa_thread_atexit_impl — C++ TLS destructor registration (stub)
 *   pthread_atfork          — glibc esporta, musl nasconde (stub)
 *   __register_atfork       — forma interna usata da alcuni runtime (stub)
 *   __stack_chk_guard       — canary stack protector
 *   __stack_chk_fail        — handler stack overflow (loop infinito)
 */

typedef unsigned long uintptr_t;

/* ── Stack protector ───────────────────────────────────────────── */

uintptr_t __stack_chk_guard = (uintptr_t)0xdeadbeefcafebabeULL;

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    for (;;)
        __asm__ volatile ("wfe");
}

/* ── Versione glibc fittizia ───────────────────────────────────── */

const char *gnu_get_libc_version(void)
{
    return "2.38";
}

const char *gnu_get_libc_release(void)
{
    return "stable";
}

/* ── __libc_start_main ─────────────────────────────────────────── */
/*
 * Firma glibc:
 *   int __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
 *
 * Chiamato da _start dei binari glibc-linked.  Su EnlilOS il processo
 * entra già con argc/argv/envp sullo stack dal loader ELF, ma i binari
 * glibc chiamano __libc_start_main esplicitamente via _start bootstrap.
 */
typedef int  (*glibc_main_fn)(int, char **, char **);
typedef void (*glibc_void_fn)(void);

int __libc_start_main(
    glibc_main_fn  main,
    int            argc,
    char         **argv,
    glibc_void_fn  init,
    glibc_void_fn  fini,
    glibc_void_fn  rtld_fini,
    void          *stack_end)
{
    (void)rtld_fini;
    (void)stack_end;
    if (init)  init();
    int ret = main(argc, argv, (char **)0);
    if (fini)  fini();
    return ret;
}

/* ── pthread_atfork / __register_atfork ────────────────────────── */
/*
 * glibc esporta pthread_atfork() direttamente; musl lo nasconde.
 * Su EnlilOS: fork in contesto multi-thread non e' supportato (EBUSY),
 * quindi questa versione e' uno stub no-op conforme a POSIX.
 */
int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void))
{
    (void)prepare;
    (void)parent;
    (void)child;
    return 0;
}

int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void *dso_handle)
{
    (void)prepare;
    (void)parent;
    (void)child;
    (void)dso_handle;
    return 0;
}

/* ── __cxa_thread_atexit_impl ──────────────────────────────────── */
/*
 * Registra distruttori C++ thread-local.  Usato da libstdc++ e libc++.
 * Stub: su EnlilOS i distruttori TLS non sono ancora eseguiti a uscita
 * thread, ma la firma deve esistere per non fallire il link runtime.
 */
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *arg,
                              void *dso_symbol)
{
    (void)dtor;
    (void)arg;
    (void)dso_symbol;
    return 0;
}
