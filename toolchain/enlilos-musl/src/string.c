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
