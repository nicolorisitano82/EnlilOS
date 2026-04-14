#ifndef ENLILOS_MUSL_SYS_STAT_H
#define ENLILOS_MUSL_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT      0170000U
#define S_IFIFO     0010000U
#define S_IFCHR     0020000U
#define S_IFDIR     0040000U
#define S_IFBLK     0060000U
#define S_IFREG     0100000U
#define S_IFLNK     0120000U
#define S_IFSOCK    0140000U

#define S_ISUID     04000U
#define S_ISGID     02000U
#define S_ISVTX     01000U

#define S_IRUSR     0400U
#define S_IWUSR     0200U
#define S_IXUSR     0100U
#define S_IRGRP     0040U
#define S_IWGRP     0020U
#define S_IXGRP     0010U
#define S_IROTH     0004U
#define S_IWOTH     0002U
#define S_IXOTH     0001U

#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
};

int    stat(const char *path, struct stat *st);
int    lstat(const char *path, struct stat *st);
int    fstat(int fd, struct stat *st);
int    fstatat(int dirfd, const char *path, struct stat *st, int flags);
int    mkdir(const char *path, mode_t mode);
int    mkfifo(const char *path, mode_t mode);
int    chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif
