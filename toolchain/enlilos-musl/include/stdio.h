#ifndef ENLILOS_MUSL_STDIO_H
#define ENLILOS_MUSL_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);

#endif
