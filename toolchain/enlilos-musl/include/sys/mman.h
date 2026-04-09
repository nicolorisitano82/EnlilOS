#ifndef ENLILOS_MUSL_SYS_MMAN_H
#define ENLILOS_MUSL_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE       0
#define PROT_READ       (1 << 0)
#define PROT_WRITE      (1 << 1)
#define PROT_EXEC       (1 << 2)

#define MAP_SHARED      (1 << 0)
#define MAP_PRIVATE     (1 << 1)
#define MAP_ANONYMOUS   (1 << 5)

#define MAP_FAILED ((void *)(-1L))

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);

#endif
