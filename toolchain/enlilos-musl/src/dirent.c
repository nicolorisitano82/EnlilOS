#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#define ENLILOS_S_IFMT   0170000U
#define ENLILOS_S_IFREG  0100000U
#define ENLILOS_S_IFDIR  0040000U

typedef struct {
    char          name[32];
    unsigned int  mode;
} enlilos_sys_dirent_t;

struct __enlilos_dirstream {
    int            fd;
    struct dirent  ent;
};

static long dirent_set_errno(long rc)
{
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return rc;
}

static unsigned char dirent_mode_to_dtype(mode_t mode)
{
    switch ((unsigned int)mode & ENLILOS_S_IFMT) {
    case ENLILOS_S_IFDIR:
        return DT_DIR;
    case ENLILOS_S_IFREG:
        return DT_REG;
    default:
        return DT_UNKNOWN;
    }
}

DIR *fdopendir(int fd)
{
    DIR *dir;

    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    dir = (DIR *)malloc(sizeof(*dir));
    if (!dir) {
        (void)close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    (void)memset(&dir->ent, 0, sizeof(dir->ent));
    return dir;
}

DIR *opendir(const char *path)
{
    int fd;

    fd = open(path ? path : ".", O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    return fdopendir(fd);
}

struct dirent *readdir(DIR *dir)
{
    enlilos_sys_dirent_t ent;
    long                 rc;

    if (!dir) {
        errno = EINVAL;
        return NULL;
    }

    rc = dirent_set_errno(user_svc3(SYS_GETDENTS, dir->fd, (long)&ent, 1));
    if (rc <= 0)
        return NULL;

    (void)memset(&dir->ent, 0, sizeof(dir->ent));
    (void)memcpy(dir->ent.d_name, ent.name, sizeof(ent.name));
    dir->ent.d_name[sizeof(dir->ent.d_name) - 1U] = '\0';
    dir->ent.d_mode = (mode_t)ent.mode;
    dir->ent.d_type = dirent_mode_to_dtype(dir->ent.d_mode);
    return &dir->ent;
}

int closedir(DIR *dir)
{
    int rc;

    if (!dir) {
        errno = EINVAL;
        return -1;
    }

    rc = close(dir->fd);
    free(dir);
    return rc;
}

void rewinddir(DIR *dir)
{
    if (!dir)
        return;
    (void)lseek(dir->fd, 0, SEEK_SET);
}

int dirfd(DIR *dir)
{
    if (!dir) {
        errno = EINVAL;
        return -1;
    }
    return dir->fd;
}
