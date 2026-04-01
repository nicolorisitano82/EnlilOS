/*
 * EnlilOS Microkernel - Runtime string/memory helpers
 *
 * Implementazioni freestanding minime per soddisfare i builtin
 * che il compilatore puo' emettere in assenza della libc.
 */

#include "types.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0U)
        *d++ = *s++;

    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t  v = (uint8_t)value;

    while (n-- > 0U)
        *d++ = v;

    return dst;
}
