#include <errno.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0U)
        return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0U; i--)
            d[i - 1U] = s[i - 1U];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0U;

    if (!s)
        return 0U;
    while (s[n] != '\0')
        n++;
    return n;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0U;

    if (!s)
        return 0U;
    while (n < maxlen && s[n] != '\0')
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    if (n == 0U)
        return 0;
    while (n > 1U && *a && *b && *a == *b) {
        a++;
        b++;
        n--;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;

    while ((*dst++ = *src++) != '\0')
        ;
    return out;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0U;

    for (; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *out = dst + strlen(dst);

    while ((*out++ = *src++) != '\0')
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char  *out = dst + strlen(dst);
    size_t i = 0U;

    while (i < n && src[i] != '\0') {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    unsigned char ch = (unsigned char)c;

    while (*s) {
        if ((unsigned char)*s == ch)
            return (char *)s;
        s++;
    }
    return ch == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char    *last = NULL;
    unsigned char  ch = (unsigned char)c;

    do {
        if ((unsigned char)*s == ch)
            last = s;
    } while (*s++ != '\0');

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0U)
        return (char *)haystack;

    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t len;
    char  *copy;

    if (!s)
        return NULL;
    len = strlen(s) + 1U;
    copy = (char *)malloc(len);
    if (!copy)
        return NULL;
    (void)memcpy(copy, s, len);
    return copy;
}

char *strerror(int errnum)
{
    switch (errnum) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such process";
    case EINTR: return "interrupted system call";
    case EIO: return "i/o error";
    case EBADF: return "bad file descriptor";
    case ECHILD: return "no child processes";
    case EAGAIN: return "resource temporarily unavailable";
    case ENOMEM: return "not enough memory";
    case EFAULT: return "bad address";
    case EBUSY: return "resource busy";
    case EEXIST: return "file exists";
    case EXDEV: return "cross-device link";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ENFILE: return "too many open files";
    case ENOTTY: return "not a tty";
    case ENOSPC: return "no space left on device";
    case ESPIPE: return "illegal seek";
    case EROFS: return "read-only filesystem";
    case EPIPE: return "broken pipe";
    case ERANGE: return "result out of range";
    case EDEADLK: return "resource deadlock avoided";
    case ENAMETOOLONG: return "name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    case ETIMEDOUT: return "timed out";
    default: return "unknown error";
    }
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0U;

    while (s[n] != '\0' && strchr(accept, s[n]) != NULL)
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0U;

    while (s[n] != '\0' && strchr(reject, s[n]) == NULL)
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        if (strchr(accept, *s) != NULL)
            return (char *)s;
        s++;
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *start;
    char *end;

    if (!saveptr || !delim)
        return NULL;

    start = str ? str : *saveptr;
    if (!start)
        return NULL;

    start += strspn(start, delim);
    if (*start == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    end = start + strcspn(start, delim);
    if (*end != '\0') {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }

    return start;
}
