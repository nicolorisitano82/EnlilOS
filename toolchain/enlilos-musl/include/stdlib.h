#ifndef ENLILOS_MUSL_STDLIB_H
#define ENLILOS_MUSL_STDLIB_H

#include <stddef.h>

void      exit(int status) __attribute__((noreturn));
void      _Exit(int status) __attribute__((noreturn));
void      abort(void) __attribute__((noreturn));
void     *malloc(size_t size);
void     *calloc(size_t nmemb, size_t size);
void     *realloc(void *ptr, size_t size);
void      free(void *ptr);
int       atoi(const char *nptr);
long      atol(const char *nptr);
double    atof(const char *nptr);
long      strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double    strtod(const char *nptr, char **endptr);
char     *getenv(const char *name);
int       setenv(const char *name, const char *value, int overwrite);
int       unsetenv(const char *name);
void      qsort(void *base, size_t nmemb, size_t size,
                int (*compar)(const void *, const void *));

#endif
