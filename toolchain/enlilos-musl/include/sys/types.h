#ifndef ENLILOS_MUSL_SYS_TYPES_H
#define ENLILOS_MUSL_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef long           ssize_t;
typedef long           off_t;
typedef long           time_t;
typedef long           clock_t;
typedef int            pid_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned int   mode_t;
typedef unsigned long  dev_t;
typedef unsigned long  ino_t;
typedef unsigned long  nlink_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef unsigned long  rlim_t;
typedef unsigned int   wint_t;
typedef struct {
    unsigned int __opaque;
} mbstate_t;

#endif
