#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    size_t map_len;
    size_t size;
} alloc_hdr_t;

static size_t alloc_align_up(size_t value, size_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

void *malloc(size_t size)
{
    alloc_hdr_t *hdr;
    size_t       need;
    void        *base;

    if (size == 0U)
        size = 1U;

    need = alloc_align_up(sizeof(*hdr) + size, 16U);
    base = mmap(NULL, need, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    hdr = (alloc_hdr_t *)base;
    hdr->map_len = need;
    hdr->size    = size;
    return (void *)(hdr + 1);
}

void free(void *ptr)
{
    alloc_hdr_t *hdr;

    if (!ptr)
        return;
    hdr = ((alloc_hdr_t *)ptr) - 1;
    (void)munmap((void *)hdr, hdr->map_len);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void  *ptr;

    if (nmemb != 0U && size > ((size_t)-1) / nmemb)
        return NULL;

    total = nmemb * size;
    ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    alloc_hdr_t *hdr;
    void        *new_ptr;
    size_t       copy_len;

    if (!ptr)
        return malloc(size);
    if (size == 0U) {
        free(ptr);
        return NULL;
    }

    hdr = ((alloc_hdr_t *)ptr) - 1;
    new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;

    copy_len = hdr->size < size ? hdr->size : size;
    memcpy(new_ptr, ptr, copy_len);
    free(ptr);
    return new_ptr;
}
