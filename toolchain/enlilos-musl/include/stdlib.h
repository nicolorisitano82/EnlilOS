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
int       atexit(void (*function)(void));
int       atoi(const char *nptr);
long      atol(const char *nptr);
double    atof(const char *nptr);
int       rand(void);
void      srand(unsigned int seed);
long      strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double    strtod(const char *nptr, char **endptr);
char     *mktemp(char *template_name);
int       mkstemp(char *template_name);
char     *mkdtemp(char *template_name);
char     *getenv(const char *name);
int       setenv(const char *name, const char *value, int overwrite);
int       unsetenv(const char *name);
void      qsort(void *base, size_t nmemb, size_t size,
                int (*compar)(const void *, const void *));
int       mblen(const char *s, size_t n);

/* PTY POSIX API */
int       posix_openpt(int flags);
int       grantpt(int fd);
int       unlockpt(int fd);
char     *ptsname(int fd);
int       ptsname_r(int fd, char *buf, size_t len);

#endif
