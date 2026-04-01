/*
 * EnlilOS Microkernel - ELF64 static loader (M6-01)
 */

#include "elf_loader.h"
#include "pmm.h"
#include "syscall.h"
#include "timer.h"
#include "vfs.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

#define EI_NIDENT          16U
#define ELFCLASS64         2U
#define ELFDATA2LSB        1U
#define EV_CURRENT         1U
#define ET_EXEC            2U
#define ET_DYN             3U
#define EM_AARCH64         183U
#define PT_LOAD            1U
#define PF_X               0x1U
#define PF_W               0x2U
#define PF_R               0x4U

#define ELF64_MAX_PHDRS    16U
#define ELF64_DYN_BASE     0x0000007FC1000000ULL
#define ELF64_HWCAP        0x3ULL /* FP | ASIMD */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    uint64_t a_type;
    uint64_t a_val;
} elf64_auxv_t;

typedef ssize_t (*elf_read_fn)(void *ctx, uint64_t off, void *dst, size_t size);

typedef struct {
    const uint8_t *data;
    size_t         size;
} elf_mem_source_t;

static char elf_last_error[96];

extern const uint8_t _binary_user_demo_elf_start[];
extern const uint8_t _binary_user_demo_elf_end[];

static void elf_set_error(const char *msg)
{
    uint32_t i = 0U;

    while (i + 1U < (uint32_t)sizeof(elf_last_error) && msg[i] != '\0') {
        elf_last_error[i] = msg[i];
        i++;
    }
    elf_last_error[i] = '\0';
}

static uint32_t elf_strlen(const char *s)
{
    uint32_t n = 0U;
    if (!s) return 0U;
    while (s[n] != '\0')
        n++;
    return n;
}

static uintptr_t elf_align_down(uintptr_t v)
{
    return v & ~(PAGE_SIZE - 1ULL);
}

static uintptr_t elf_align_up(uintptr_t v)
{
    return (v + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static ssize_t elf_mem_read(void *ctx, uint64_t off, void *dst, size_t size)
{
    elf_mem_source_t *src = (elf_mem_source_t *)ctx;

    if (!src || !dst) return -1;
    if (off > src->size || size > src->size - off)
        return -1;

    memcpy(dst, src->data + off, size);
    return (ssize_t)size;
}

static ssize_t elf_vfs_read(void *ctx, uint64_t off, void *dst, size_t size)
{
    vfs_file_t *file = (vfs_file_t *)ctx;
    ssize_t rc;

    if (!file || !dst) return -1;
    file->pos = off;
    rc = vfs_read(file, dst, size);
    return rc;
}

static int elf_read_exact(elf_read_fn rd, void *ctx, uint64_t off,
                          void *dst, size_t size)
{
    ssize_t rc = rd(ctx, off, dst, size);
    return (rc == (ssize_t)size) ? 0 : -1;
}

static int elf_compute_bias(const elf64_ehdr_t *ehdr,
                            const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                            uintptr_t *bias_out,
                            uintptr_t *image_lo_out,
                            uintptr_t *image_hi_out)
{
    uintptr_t min_va = 0ULL;
    uintptr_t max_va = 0ULL;
    uint8_t   have_load = 0U;
    uintptr_t bias = 0ULL;
    uintptr_t stack_base = MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE;

    for (uint32_t i = 0U; i < ehdr->e_phnum; i++) {
        uintptr_t seg_lo, seg_hi;

        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0ULL)
            continue;

        seg_lo = elf_align_down((uintptr_t)phdrs[i].p_vaddr);
        seg_hi = elf_align_up((uintptr_t)(phdrs[i].p_vaddr + phdrs[i].p_memsz));
        if (!have_load || seg_lo < min_va) min_va = seg_lo;
        if (!have_load || seg_hi > max_va) max_va = seg_hi;
        have_load = 1U;
    }

    if (!have_load) {
        elf_set_error("ELF senza segmenti PT_LOAD");
        return -1;
    }

    if (ehdr->e_type == ET_DYN)
        bias = ELF64_DYN_BASE - min_va;

    if (min_va + bias < MMU_USER_BASE || max_va + bias > stack_base) {
        elf_set_error("range ELF fuori dal window user");
        return -1;
    }

    *bias_out = bias;
    *image_lo_out = min_va + bias;
    *image_hi_out = max_va + bias;
    return 0;
}

static int elf_validate_header(const elf64_ehdr_t *ehdr, size_t image_size)
{
    if (!ehdr) return -1;

    if (image_size < sizeof(*ehdr)) {
        elf_set_error("ELF troppo piccolo");
        return -1;
    }

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        elf_set_error("magic ELF non valida");
        return -1;
    }

    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_ident[6] != EV_CURRENT) {
        elf_set_error("ELF64 little-endian richiesto");
        return -1;
    }

    if (ehdr->e_machine != EM_AARCH64) {
        elf_set_error("ELF non AArch64");
        return -1;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        elf_set_error("tipo ELF non supportato");
        return -1;
    }

    if (ehdr->e_phnum == 0U || ehdr->e_phnum > ELF64_MAX_PHDRS ||
        ehdr->e_phentsize != sizeof(elf64_phdr_t)) {
        elf_set_error("program header table non supportata");
        return -1;
    }

    if (ehdr->e_phoff > image_size ||
        (uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t) > image_size - ehdr->e_phoff) {
        elf_set_error("program header fuori file");
        return -1;
    }

    return 0;
}

static uintptr_t elf_phdr_runtime_va(const elf64_ehdr_t *ehdr,
                                     const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                                     uintptr_t bias)
{
    uint64_t off = ehdr->e_phoff;

    for (uint32_t i = 0U; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        if (off >= phdrs[i].p_offset &&
            off + (uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t) <=
                phdrs[i].p_offset + phdrs[i].p_filesz) {
            return (uintptr_t)(phdrs[i].p_vaddr + bias + (off - phdrs[i].p_offset));
        }
    }

    return 0ULL;
}

static int elf_build_stack(mm_space_t *space,
                           const char *const *argv, uint64_t argc,
                           const char *const *envp, uint64_t envc,
                           const elf64_ehdr_t *ehdr,
                           const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                           uintptr_t bias, elf_image_t *out)
{
    uintptr_t arg_ptrs[ELF_LOADER_MAX_ARGS];
    uintptr_t env_ptrs[ELF_LOADER_MAX_ENVP];
    uintptr_t sp = MMU_USER_STACK_TOP;
    uintptr_t phdr_va;
    uintptr_t argv_va;
    uintptr_t envp_va;
    uintptr_t auxv_va;
    elf64_auxv_t auxv[7];
    uint32_t auxc = 0U;
    uint32_t words;
    size_t   table_bytes;
    uint64_t *stack_words;

    if (argc > ELF_LOADER_MAX_ARGS || envc > ELF_LOADER_MAX_ENVP) {
        elf_set_error("argc/envc oltre limite loader");
        return -1;
    }

    for (uint64_t i = envc; i > 0ULL; i--) {
        const char *s = envp[i - 1U] ? envp[i - 1U] : "";
        uint32_t len = elf_strlen(s) + 1U;
        void *dst;

        sp -= len;
        dst = mmu_space_resolve_ptr(space, sp, len);
        if (!dst) {
            elf_set_error("env string fuori mapping");
            return -1;
        }
        memcpy(dst, s, len);
        env_ptrs[i - 1U] = sp;
    }

    for (uint64_t i = argc; i > 0ULL; i--) {
        const char *s = argv[i - 1U] ? argv[i - 1U] : "";
        uint32_t len = elf_strlen(s) + 1U;
        void *dst;

        sp -= len;
        dst = mmu_space_resolve_ptr(space, sp, len);
        if (!dst) {
            elf_set_error("argv string fuori mapping");
            return -1;
        }
        memcpy(dst, s, len);
        arg_ptrs[i - 1U] = sp;
    }

    sp &= ~0xFULL;
    phdr_va = elf_phdr_runtime_va(ehdr, phdrs, bias);
    if (phdr_va == 0ULL) {
        elf_set_error("AT_PHDR non risolvibile");
        return -1;
    }

    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHDR,   phdr_va };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHENT,  ehdr->e_phentsize };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHNUM,  ehdr->e_phnum };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_ENTRY,  ehdr->e_entry + bias };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PAGESZ, (uint64_t)PAGE_SIZE };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_HWCAP,  ELF64_HWCAP };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_NULL,   0ULL };

    words = 1U + (uint32_t)argc + 1U + (uint32_t)envc + 1U + auxc * 2U;
    table_bytes = (size_t)words * sizeof(uint64_t);
    sp -= table_bytes;
    sp &= ~0xFULL;

    stack_words = (uint64_t *)mmu_space_resolve_ptr(space, sp, table_bytes);
    if (!stack_words) {
        elf_set_error("stack ABI fuori mapping");
        return -1;
    }

    stack_words[0] = argc;
    argv_va = sp + sizeof(uint64_t);

    for (uint64_t i = 0ULL; i < argc; i++)
        stack_words[1U + i] = arg_ptrs[i];
    stack_words[1U + argc] = 0ULL;

    envp_va = argv_va + ((uintptr_t)argc + 1ULL) * sizeof(uint64_t);
    for (uint64_t i = 0ULL; i < envc; i++)
        stack_words[2U + argc + i] = env_ptrs[i];
    stack_words[2U + argc + envc] = 0ULL;

    auxv_va = envp_va + ((uintptr_t)envc + 1ULL) * sizeof(uint64_t);
    {
        uint32_t base = 3U + (uint32_t)argc + (uint32_t)envc;
        for (uint32_t i = 0U; i < auxc; i++) {
            stack_words[base + i * 2U] = auxv[i].a_type;
            stack_words[base + i * 2U + 1U] = auxv[i].a_val;
        }
    }

    out->argc     = argc;
    out->argv     = argv_va;
    out->envp     = envp_va;
    out->auxv     = auxv_va;
    out->user_sp  = sp;
    out->phdr     = phdr_va;
    out->stack_base = MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE;
    out->stack_top  = MMU_USER_STACK_TOP;
    return 0;
}

static int elf_prefault_loaded_image(mm_space_t *space,
                                     const elf64_ehdr_t *ehdr,
                                     const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                                     uintptr_t bias)
{
    uintptr_t stack_base = MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE;

    for (uint32_t i = 0U; i < ehdr->e_phnum; i++) {
        uintptr_t seg_lo, seg_hi;

        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0ULL)
            continue;

        seg_lo = elf_align_down((uintptr_t)phdrs[i].p_vaddr + bias);
        seg_hi = elf_align_up((uintptr_t)(phdrs[i].p_vaddr + bias + phdrs[i].p_memsz));
        if (mmu_prefault_space_range(space, seg_lo, seg_hi) < 0) {
            elf_set_error("prefault segmento fallito");
            return -1;
        }
    }

    if (mmu_prefault_space_range(space, stack_base, MMU_USER_STACK_TOP) < 0) {
        elf_set_error("prefault stack fallito");
        return -1;
    }

    return 0;
}

static int elf_load_common(elf_read_fn rd, void *ctx, size_t image_size,
                           const char *const *argv, uint64_t argc,
                           const char *const *envp, uint64_t envc,
                           elf_image_t *out)
{
    elf64_ehdr_t ehdr;
    elf64_phdr_t phdrs[ELF64_MAX_PHDRS];
    elf_image_t  image;
    uintptr_t    bias;
    uintptr_t    image_lo;
    uintptr_t    image_hi;
    mm_space_t  *space;

    if (!out) {
        elf_set_error("output ELF nullo");
        return -1;
    }
    memset(&image, 0, sizeof(image));

    if (elf_read_exact(rd, ctx, 0ULL, &ehdr, sizeof(ehdr)) < 0) {
        elf_set_error("read ELF header fallita");
        return -1;
    }
    if (elf_validate_header(&ehdr, image_size) < 0)
        return -1;
    if (elf_read_exact(rd, ctx, ehdr.e_phoff, phdrs,
                       (size_t)ehdr.e_phnum * sizeof(elf64_phdr_t)) < 0) {
        elf_set_error("read program headers fallita");
        return -1;
    }
    if (elf_compute_bias(&ehdr, phdrs, &bias, &image_lo, &image_hi) < 0)
        return -1;

    space = mmu_space_create();
    if (!space) {
        elf_set_error("mm_space esauriti");
        return -1;
    }

    for (uint32_t i = 0U; i < ehdr.e_phnum; i++) {
        uintptr_t seg_va;
        uintptr_t seg_lo;
        uintptr_t seg_hi;
        uint32_t  prot = 0U;
        uint64_t  copied = 0ULL;

        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0ULL)
            continue;
        if (phdrs[i].p_filesz > phdrs[i].p_memsz) {
            elf_set_error("p_filesz > p_memsz");
            goto fail;
        }
        if (phdrs[i].p_offset > image_size ||
            phdrs[i].p_filesz > image_size - phdrs[i].p_offset) {
            elf_set_error("segmento fuori file");
            goto fail;
        }

        seg_va = (uintptr_t)phdrs[i].p_vaddr + bias;
        seg_lo = elf_align_down(seg_va);
        seg_hi = elf_align_up(seg_va + phdrs[i].p_memsz);

        if (phdrs[i].p_flags & PF_R) prot |= MMU_PROT_USER_R;
        if (phdrs[i].p_flags & PF_W) prot |= MMU_PROT_USER_W;
        if (phdrs[i].p_flags & PF_X) prot |= MMU_PROT_USER_X;
        if (prot == 0U) prot = MMU_PROT_USER_R;

        if (mmu_map_user_region(space, seg_lo, seg_hi - seg_lo, prot) < 0) {
            elf_set_error("map PT_LOAD fallita");
            goto fail;
        }

        while (copied < phdrs[i].p_filesz) {
            size_t chunk = 512U;
            void  *dst;

            if ((uint64_t)chunk > phdrs[i].p_filesz - copied)
                chunk = (size_t)(phdrs[i].p_filesz - copied);

            dst = mmu_space_resolve_ptr(space, seg_va + copied, chunk);
            if (!dst) {
                elf_set_error("copy PT_LOAD fuori mapping");
                goto fail;
            }

            if (elf_read_exact(rd, ctx, phdrs[i].p_offset + copied, dst, chunk) < 0) {
                elf_set_error("read PT_LOAD fallita");
                goto fail;
            }
            copied += chunk;
        }
    }

    if (mmu_map_user_region(space,
                            MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE,
                            MMU_USER_STACK_SIZE,
                            MMU_PROT_USER_R | MMU_PROT_USER_W) < 0) {
        elf_set_error("map stack user fallita");
        goto fail;
    }

    image.space     = space;
    image.entry     = (uintptr_t)ehdr.e_entry + bias;
    image.image_base = image_lo;
    image.image_end  = image_hi;
    image.phentsize = ehdr.e_phentsize;
    image.phnum     = ehdr.e_phnum;

    if (elf_build_stack(space, argv, argc, envp, envc, &ehdr, phdrs, bias, &image) < 0)
        goto fail;
    if (elf_prefault_loaded_image(space, &ehdr, phdrs, bias) < 0)
        goto fail;

    *out = image;
    elf_set_error("OK");
    return 0;

fail:
    mmu_space_destroy(space);
    return -1;
}

int elf64_load_from_memory(const void *image, size_t image_size,
                           const char *argv0, elf_image_t *out)
{
    elf_mem_source_t src;
    const char *argv_local[1];

    if (!image) {
        elf_set_error("blob ELF nullo");
        return -1;
    }

    src.data = (const uint8_t *)image;
    src.size = image_size;
    argv_local[0] = (argv0 && argv0[0] != '\0') ? argv0 : "program";
    return elf_load_common(elf_mem_read, &src, image_size,
                           argv_local, 1ULL, NULL, 0ULL, out);
}

int elf64_load_from_path(const char *path, const char *argv0, elf_image_t *out)
{
    vfs_file_t file;
    stat_t     st;
    int        rc;
    const char *argv_local[1];

    if (!path || path[0] == '\0') {
        elf_set_error("path ELF vuoto");
        return -1;
    }

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0) {
        elf_set_error("open ELF fallita");
        return -1;
    }

    rc = vfs_stat(&file, &st);
    if (rc < 0) {
        (void)vfs_close(&file);
        elf_set_error("stat ELF fallita");
        return -1;
    }

    argv_local[0] = (argv0 && argv0[0] != '\0') ? argv0 : path;
    rc = elf_load_common(elf_vfs_read, &file, (size_t)st.st_size,
                         argv_local, 1ULL, NULL, 0ULL, out);
    (void)vfs_close(&file);
    return rc;
}

int elf64_load_from_path_exec(const char *path,
                              const char *const *argv, uint64_t argc,
                              const char *const *envp, uint64_t envc,
                              elf_image_t *out)
{
    vfs_file_t file;
    stat_t     st;
    int        rc;
    const char *fallback_argv[1];

    if (!path || path[0] == '\0') {
        elf_set_error("path ELF vuoto");
        return -1;
    }

    if (!argv || argc == 0ULL) {
        fallback_argv[0] = path;
        argv = fallback_argv;
        argc = 1ULL;
    }

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0) {
        elf_set_error("open ELF fallita");
        return -1;
    }

    rc = vfs_stat(&file, &st);
    if (rc < 0) {
        (void)vfs_close(&file);
        elf_set_error("stat ELF fallita");
        return -1;
    }

    rc = elf_load_common(elf_vfs_read, &file, (size_t)st.st_size,
                         argv, argc, envp, envc, out);
    (void)vfs_close(&file);
    return rc;
}

void elf64_unload_image(elf_image_t *image)
{
    if (!image) return;
    if (image->space)
        mmu_space_destroy(image->space);
    memset(image, 0, sizeof(*image));
}

sched_tcb_t *elf64_spawn_image(const char *task_name, const elf_image_t *image,
                               uint8_t priority)
{
    if (!image || !image->space || image->entry == 0ULL || image->user_sp == 0ULL) {
        elf_set_error("immagine ELF non pronta");
        return NULL;
    }

    return sched_task_create_user(task_name ? task_name : "user-elf",
                                  image->space,
                                  image->entry,
                                  image->user_sp,
                                  image->argc,
                                  image->argv,
                                  image->envp,
                                  image->auxv,
                                  priority);
}

int elf64_spawn_path(const char *path, const char *argv0,
                     uint8_t priority, uint32_t *pid_out)
{
    elf_image_t   image;
    sched_tcb_t  *task;

    if (elf64_load_from_path(path, argv0, &image) < 0)
        return -1;

    task = elf64_spawn_image("user-elf", &image, priority);
    if (!task) {
        elf64_unload_image(&image);
        elf_set_error("spawn ELF fallita");
        return -1;
    }

    if (pid_out) *pid_out = task->pid;
    return 0;
}

int elf64_spawn_demo(uint8_t priority, uint32_t *pid_out)
{
    elf_image_t image;
    size_t      size = (size_t)(_binary_user_demo_elf_end - _binary_user_demo_elf_start);
    sched_tcb_t *task;

    if (elf64_load_from_memory(_binary_user_demo_elf_start, size,
                               "demo.elf", &image) < 0)
        return -1;

    task = elf64_spawn_image("elf-demo", &image, priority);
    if (!task) {
        elf64_unload_image(&image);
        elf_set_error("spawn demo ELF fallita");
        return -1;
    }

    if (pid_out) *pid_out = task->pid;
    return 0;
}

const char *elf64_last_error(void)
{
    return elf_last_error[0] ? elf_last_error : "ELF loader";
}
