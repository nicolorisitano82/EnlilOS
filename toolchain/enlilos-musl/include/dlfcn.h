#ifndef ENLILOS_MUSL_DLFCN_H
#define ENLILOS_MUSL_DLFCN_H

#include <stddef.h>

#define RTLD_LOCAL   0x0000
#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_GLOBAL  0x0100

void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);

#endif
