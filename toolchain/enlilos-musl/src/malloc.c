#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MALLOC_ALIGN       16U
#define MALLOC_PAGE        4096U
#define MALLOC_ARENA_CHUNK (64U * 1024U)
#define BLOCK_MAGIC        0x4D414C4CU

typedef struct malloc_arena malloc_arena_t;
typedef struct malloc_block malloc_block_t;

struct malloc_arena {
    malloc_arena_t *next;
    size_t          map_len;
};

struct malloc_block {
    malloc_arena_t *arena;
    size_t          size;
    size_t          prev_size;
    uint32_t        free;
    uint32_t        magic;
};

#define ARENA_HDR_SIZE (((sizeof(malloc_arena_t) + MALLOC_ALIGN - 1U) / MALLOC_ALIGN) * MALLOC_ALIGN)
#define BLOCK_HDR_SIZE (((sizeof(malloc_block_t) + MALLOC_ALIGN - 1U) / MALLOC_ALIGN) * MALLOC_ALIGN)

static malloc_arena_t *g_malloc_arenas;
static volatile int    g_malloc_lock;

static size_t malloc_align_up(size_t value, size_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static void malloc_lock(void)
{
    while (__sync_lock_test_and_set(&g_malloc_lock, 1) != 0)
        ;
}

static void malloc_unlock(void)
{
    __sync_lock_release(&g_malloc_lock);
}

static char *malloc_arena_end(malloc_arena_t *arena)
{
    return (char *)arena + arena->map_len;
}

static malloc_block_t *malloc_arena_first(malloc_arena_t *arena)
{
    return (malloc_block_t *)((char *)arena + ARENA_HDR_SIZE);
}

static malloc_block_t *malloc_block_next(malloc_block_t *block)
{
    char *next;

    if (!block || !block->arena)
        return NULL;

    next = (char *)block + BLOCK_HDR_SIZE + block->size;
    if (next + BLOCK_HDR_SIZE > malloc_arena_end(block->arena))
        return NULL;
    return (malloc_block_t *)next;
}

static malloc_block_t *malloc_block_prev(malloc_block_t *block)
{
    char *prev;

    if (!block || !block->arena || block->prev_size == 0U)
        return NULL;

    prev = (char *)block - BLOCK_HDR_SIZE - block->prev_size;
    if (prev < (char *)malloc_arena_first(block->arena))
        return NULL;
    return (malloc_block_t *)prev;
}

static void malloc_block_init(malloc_block_t *block, malloc_arena_t *arena,
                              size_t size, size_t prev_size, uint32_t free_flag)
{
    block->arena     = arena;
    block->size      = size;
    block->prev_size = prev_size;
    block->free      = free_flag;
    block->magic     = BLOCK_MAGIC;
}

static malloc_block_t *malloc_arena_alloc(size_t need)
{
    malloc_arena_t *arena;
    malloc_block_t *block;
    size_t          map_len;
    size_t          usable;

    map_len = ARENA_HDR_SIZE + BLOCK_HDR_SIZE + need;
    if (map_len < MALLOC_ARENA_CHUNK)
        map_len = MALLOC_ARENA_CHUNK;
    map_len = malloc_align_up(map_len, MALLOC_PAGE);

    arena = (malloc_arena_t *)mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        errno = ENOMEM;
        return NULL;
    }

    arena->map_len = map_len;
    arena->next = g_malloc_arenas;
    g_malloc_arenas = arena;

    usable = map_len - ARENA_HDR_SIZE - BLOCK_HDR_SIZE;
    block = malloc_arena_first(arena);
    malloc_block_init(block, arena, usable, 0U, 1U);
    return block;
}

static void malloc_block_split(malloc_block_t *block, size_t need)
{
    malloc_block_t *next;
    size_t          remain;

    if (!block || block->size < need)
        return;

    remain = block->size - need;
    if (remain <= BLOCK_HDR_SIZE + MALLOC_ALIGN)
        return;

    next = (malloc_block_t *)((char *)block + BLOCK_HDR_SIZE + need);
    malloc_block_init(next, block->arena,
                      remain - BLOCK_HDR_SIZE,
                      need, 1U);
    block->size = need;

    {
        malloc_block_t *after = malloc_block_next(next);
        if (after)
            after->prev_size = next->size;
    }
}

static void malloc_block_coalesce_with_next(malloc_block_t *block)
{
    malloc_block_t *next;

    if (!block)
        return;

    next = malloc_block_next(block);
    if (!next || next->magic != BLOCK_MAGIC || !next->free)
        return;

    block->size += BLOCK_HDR_SIZE + next->size;
    {
        malloc_block_t *after = malloc_block_next(block);
        if (after)
            after->prev_size = block->size;
    }
}

static void malloc_block_free_locked(malloc_block_t *block)
{
    malloc_block_t *prev;

    if (!block || block->magic != BLOCK_MAGIC)
        return;

    block->free = 1U;
    malloc_block_coalesce_with_next(block);

    prev = malloc_block_prev(block);
    if (prev && prev->magic == BLOCK_MAGIC && prev->free) {
        malloc_block_coalesce_with_next(prev);
        block = prev;
    }

    {
        malloc_block_t *next = malloc_block_next(block);
        if (next)
            next->prev_size = block->size;
    }
}

static void *malloc_locked(size_t size)
{
    malloc_arena_t *arena;
    malloc_block_t *block;
    size_t          need;

    if (size == 0U)
        size = 1U;
    need = malloc_align_up(size, MALLOC_ALIGN);

    for (arena = g_malloc_arenas; arena; arena = arena->next) {
        for (block = malloc_arena_first(arena);
             block && (char *)block < malloc_arena_end(arena);
             block = malloc_block_next(block)) {
            if (block->magic != BLOCK_MAGIC)
                return NULL;
            if (!block->free || block->size < need)
                continue;

            malloc_block_split(block, need);
            block->free = 0U;
            {
                malloc_block_t *next = malloc_block_next(block);
                if (next)
                    next->prev_size = block->size;
            }
            return (char *)block + BLOCK_HDR_SIZE;
        }
    }

    block = malloc_arena_alloc(need);
    if (!block)
        return NULL;

    malloc_block_split(block, need);
    block->free = 0U;
    {
        malloc_block_t *next = malloc_block_next(block);
        if (next)
            next->prev_size = block->size;
    }
    return (char *)block + BLOCK_HDR_SIZE;
}

void *malloc(size_t size)
{
    void *ptr;

    malloc_lock();
    ptr = malloc_locked(size);
    malloc_unlock();

    if (!ptr)
        errno = ENOMEM;
    return ptr;
}

void free(void *ptr)
{
    malloc_block_t *block;

    if (!ptr)
        return;

    block = (malloc_block_t *)((char *)ptr - BLOCK_HDR_SIZE);
    malloc_lock();
    malloc_block_free_locked(block);
    malloc_unlock();
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void  *ptr;

    if (nmemb != 0U && size > ((size_t)-1) / nmemb) {
        errno = ENOMEM;
        return NULL;
    }

    total = nmemb * size;
    ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    malloc_block_t *block;
    void           *new_ptr;
    size_t          need;

    if (!ptr)
        return malloc(size);
    if (size == 0U) {
        free(ptr);
        return NULL;
    }

    need = malloc_align_up(size, MALLOC_ALIGN);
    block = (malloc_block_t *)((char *)ptr - BLOCK_HDR_SIZE);

    malloc_lock();
    if (block->magic == BLOCK_MAGIC && block->size >= need) {
        malloc_block_split(block, need);
        block->free = 0U;
        malloc_unlock();
        return ptr;
    }
    malloc_unlock();

    new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;

    if (block->magic == BLOCK_MAGIC) {
        size_t copy_len = block->size < size ? block->size : size;
        memcpy(new_ptr, ptr, copy_len);
    }
    free(ptr);
    return new_ptr;
}
