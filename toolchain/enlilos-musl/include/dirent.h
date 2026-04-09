#ifndef ENLILOS_MUSL_DIRENT_H
#define ENLILOS_MUSL_DIRENT_H

#include <sys/types.h>

#define DT_UNKNOWN  0
#define DT_REG      8
#define DT_DIR      4

struct dirent {
    char           d_name[32];
    unsigned char  d_type;
    mode_t         d_mode;
};

typedef struct __enlilos_dirstream DIR;

DIR            *opendir(const char *path);
DIR            *fdopendir(int fd);
struct dirent  *readdir(DIR *dir);
int             closedir(DIR *dir);
void            rewinddir(DIR *dir);
int             dirfd(DIR *dir);

#endif
