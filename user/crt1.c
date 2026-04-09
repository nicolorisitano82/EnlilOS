#include "crt_runtime.h"
#include "syscall.h"
#include "user_svc.h"

typedef void (*crt_hook_t)(void);

extern int main(int argc, char **argv, char **envp);
extern void _init(void);
extern void _fini(void);
extern void __enlilos_thread_runtime_init(void) __attribute__((weak));

extern crt_hook_t __preinit_array_start[];
extern crt_hook_t __preinit_array_end[];
extern crt_hook_t __init_array_start[];
extern crt_hook_t __init_array_end[];
extern crt_hook_t __fini_array_start[];
extern crt_hook_t __fini_array_end[];

char       **environ = (char **)0;
const void *__enlilos_auxv = (const void *)0;

static void crt_run_hooks_forward(crt_hook_t *begin, crt_hook_t *end)
{
    for (crt_hook_t *it = begin; it < end; it++) {
        if (*it)
            (*it)();
    }
}

static void crt_run_hooks_reverse(crt_hook_t *begin, crt_hook_t *end)
{
    while (end > begin) {
        end--;
        if (*end)
            (*end)();
    }
}

__attribute__((noreturn)) void _start(long argc, char **argv, char **envp, void *auxv)
{
    int rc;

    environ = envp;
    __enlilos_auxv = auxv;
    if (__enlilos_thread_runtime_init)
        __enlilos_thread_runtime_init();

    crt_run_hooks_forward(__preinit_array_start, __preinit_array_end);
    _init();
    crt_run_hooks_forward(__init_array_start, __init_array_end);

    rc = main((int)argc, argv, envp);

    crt_run_hooks_reverse(__fini_array_start, __fini_array_end);
    _fini();
    user_svc_exit(rc, SYS_EXIT);
}
