#ifndef ENLILOS_MUSL_STDIO_H
#define ENLILOS_MUSL_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

typedef struct __enlilos_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *tmpfile(void);
int   fclose(FILE *stream);
int   fflush(FILE *stream);
int   fileno(FILE *stream);
int   ferror(FILE *stream);
int   feof(FILE *stream);
void  clearerr(FILE *stream);
void  rewind(FILE *stream);
int   fgetc(FILE *stream);
int   fputc(int c, FILE *stream);
int   fputs(const char *s, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int   fprintf(FILE *stream, const char *fmt, ...);
int   vfprintf(FILE *stream, const char *fmt, va_list ap);
int   perror(const char *s);
int   sscanf(const char *str, const char *fmt, ...);
int   remove(const char *path);
int   rename(const char *oldpath, const char *newpath);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);

#endif
