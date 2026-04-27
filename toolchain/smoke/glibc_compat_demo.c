/*
 * glibc_compat_demo — smoke test M11-05g
 *
 * Verifica:
 *   1. dlopen("/GLIBC-COMPAT.SO") → handle valido
 *   2. dlsym → gnu_get_libc_version() ritorna "2.38"
 *   3. dlsym → __libc_start_main non NULL
 *   4. dlsym → __stack_chk_guard non NULL, valore non zero
 *   5. dlsym → pthread_atfork non NULL
 *   6. dlsym → __cxa_thread_atexit_impl non NULL
 *   7. dlclose → OK
 *
 * Output: /data/GLIBCCOMPAT.TXT = "glibc-compat-ok\n"
 */

#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

static void write_result(const char *msg)
{
    int fd = open("/data/GLIBCCOMPAT.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
}

static void fail(const char *reason)
{
    write_result(reason);
    _exit(1);
}

int main(void)
{
    void       *handle;
    const char *(*get_ver)(void);
    unsigned long *guard_ptr;
    void        *sym;

    /* ── 1. dlopen ────────────────────────────────────────────── */
    handle = dlopen("/GLIBC-COMPAT.SO", RTLD_NOW);
    if (!handle) fail("dlopen-fail\n");

    /* ── 2. gnu_get_libc_version → "2.38" ─────────────────────── */
    *(void **)&get_ver = dlsym(handle, "gnu_get_libc_version");
    if (!get_ver)           fail("dlsym-gnu_get_libc_version-fail\n");
    if (strcmp(get_ver(), "2.38") != 0)
                            fail("gnu_get_libc_version-wrong\n");

    /* ── 3. __libc_start_main presente ────────────────────────── */
    sym = dlsym(handle, "__libc_start_main");
    if (!sym)               fail("dlsym-libc_start_main-fail\n");

    /* ── 4. __stack_chk_guard non zero ────────────────────────── */
    *(void **)&guard_ptr = dlsym(handle, "__stack_chk_guard");
    if (!guard_ptr)         fail("dlsym-stack_chk_guard-fail\n");
    if (*guard_ptr == 0UL)  fail("stack_chk_guard-zero\n");

    /* ── 5. pthread_atfork presente ───────────────────────────── */
    sym = dlsym(handle, "pthread_atfork");
    if (!sym)               fail("dlsym-pthread_atfork-fail\n");

    /* ── 6. __cxa_thread_atexit_impl presente ─────────────────── */
    sym = dlsym(handle, "__cxa_thread_atexit_impl");
    if (!sym)               fail("dlsym-cxa_thread_atexit_impl-fail\n");

    /* ── 7. dlclose ───────────────────────────────────────────── */
    if (dlclose(handle) != 0) fail("dlclose-fail\n");

    write_result("glibc-compat-ok\n");
    return 0;
}
