#ifndef ENLILOS_MUSL_STDLIB_H
#define ENLILOS_MUSL_STDLIB_H

#include <stddef.h>

void   exit(int status) __attribute__((noreturn));
void   _Exit(int status) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

#endif
