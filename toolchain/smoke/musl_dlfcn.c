#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

typedef const char *(*dyn_msg_fn_t)(void);
typedef unsigned long (*dyn_len_fn_t)(void);

static int write_full(int fd, const void *buf, unsigned long len)
{
    const char *cursor = (const char *)buf;

    while (len > 0UL) {
        ssize_t rc = write(fd, cursor, (size_t)len);

        if (rc <= 0)
            return -1;
        cursor += (unsigned long)rc;
        len -= (unsigned long)rc;
    }
    return 0;
}

int main(void)
{
    void       *handle;
    dyn_msg_fn_t dyn_msg;
    dyn_len_fn_t dyn_len;
    const char *msg;
    unsigned long len;
    int fd;

    handle = dlopen("/libdyn.so", RTLD_NOW);
    if (!handle)
        return 1;

    dyn_msg = (dyn_msg_fn_t)dlsym(handle, "dyn_msg");
    if (!dyn_msg) {
        (void)dlclose(handle);
        return 2;
    }
    dyn_len = (dyn_len_fn_t)dlsym(handle, "dyn_len");
    if (!dyn_len) {
        (void)dlclose(handle);
        return 3;
    }

    fd = open("/data/MUSLDL.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        (void)dlclose(handle);
        return 4;
    }

    msg = dyn_msg();
    len = dyn_len();
    if (!msg || len == 0UL || write_full(fd, msg, len) < 0) {
        (void)close(fd);
        (void)dlclose(handle);
        return 5;
    }

    if (close(fd) < 0) {
        (void)dlclose(handle);
        return 6;
    }

    if (dlclose(handle) != 0)
        return 7;

    return 0;
}
