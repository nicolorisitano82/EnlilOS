#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

static __thread char dl_last_error_buf[128];
static __thread int  dl_last_error_ready;

static void dl_clear_local_error(void)
{
    dl_last_error_buf[0] = '\0';
    dl_last_error_ready = 0;
}

static void dl_capture_kernel_error(const char *fallback_prefix)
{
    long rc = user_svc2(SYS_DLERROR, (long)dl_last_error_buf,
                        (long)sizeof(dl_last_error_buf));

    if (rc < 0 || dl_last_error_buf[0] == '\0') {
        snprintf(dl_last_error_buf, sizeof(dl_last_error_buf),
                 "%s: %s",
                 fallback_prefix ? fallback_prefix : "libdl",
                 strerror(errno));
    }
    dl_last_error_ready = 1;
}

void *dlopen(const char *path, int mode)
{
    long rc;

    if (!path) {
        errno = EFAULT;
        dl_capture_kernel_error("dlopen");
        return NULL;
    }

    rc = user_svc2(SYS_DLOPEN, (long)path, mode);
    if (rc < 0) {
        errno = (int)(-rc);
        dl_capture_kernel_error("dlopen");
        return NULL;
    }

    dl_clear_local_error();
    return (void *)(uintptr_t)rc;
}

void *dlsym(void *handle, const char *symbol)
{
    long rc;

    if (!handle || !symbol) {
        errno = EFAULT;
        dl_capture_kernel_error("dlsym");
        return NULL;
    }

    rc = user_svc2(SYS_DLSYM, (long)(uintptr_t)handle, (long)symbol);
    if (rc < 0) {
        errno = (int)(-rc);
        dl_capture_kernel_error("dlsym");
        return NULL;
    }

    dl_clear_local_error();
    return (void *)(uintptr_t)rc;
}

int dlclose(void *handle)
{
    long rc;

    if (!handle) {
        errno = EFAULT;
        dl_capture_kernel_error("dlclose");
        return -1;
    }

    rc = user_svc1(SYS_DLCLOSE, (long)(uintptr_t)handle);
    if (rc < 0) {
        errno = (int)(-rc);
        dl_capture_kernel_error("dlclose");
        return -1;
    }

    dl_clear_local_error();
    return 0;
}

char *dlerror(void)
{
    if (!dl_last_error_ready || dl_last_error_buf[0] == '\0')
        return NULL;

    dl_last_error_ready = 0;
    return dl_last_error_buf;
}
