/*
 * EnlilOS Microkernel - ELF64 loader (M6-01 / M6-03)
 *
 * M6-01: loader statico per ELF64 AArch64.
 * M6-03: supporto kernel-side a PT_INTERP + DT_NEEDED per PIE/DSO semplici.
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
#define PT_DYNAMIC         2U
#define PT_INTERP          3U
#define PT_TLS             7U   /* Thread-Local Storage template (M11-01b) */
#define PF_X               0x1U
#define PF_W               0x2U
#define PF_R               0x4U

#define DT_NULL            0ULL
#define DT_NEEDED          1ULL
#define DT_HASH            4ULL
#define DT_STRTAB          5ULL
#define DT_SYMTAB          6ULL
#define DT_RELA            7ULL
#define DT_RELASZ          8ULL
#define DT_RELAENT         9ULL
#define DT_STRSZ           10ULL
#define DT_SYMENT          11ULL
#define DT_PLTGOT          3ULL
#define DT_PLTREL          20ULL
#define DT_DEBUG           21ULL
#define DT_JMPREL          23ULL
#define DT_PLTRELSZ        2ULL

#define SHN_UNDEF          0U
#define STB_WEAK           2U

#define R_AARCH64_GLOB_DAT 1025ULL
#define R_AARCH64_JUMP_SLOT 1026ULL
#define R_AARCH64_RELATIVE 1027ULL

#define ELF64_MAX_PHDRS    16U
#define ELF64_DYN_BASE     0x0000007FC2000000ULL
#define ELF64_HWCAP        0x3ULL /* FP | ASIMD */
#define ELF64_MAX_OBJECTS  8U
#define ELF64_MAX_NEEDED   8U
#define ELF64_NAME_MAX     64U

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

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} elf64_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

typedef ssize_t (*elf_read_fn)(void *ctx, uint64_t off, void *dst, size_t size);

typedef struct {
    const uint8_t *data;
    size_t         size;
} elf_mem_source_t;

typedef struct {
    uintptr_t dynsym_va;
    uintptr_t dynstr_va;
    uintptr_t hash_va;
    uintptr_t rela_va;
    uintptr_t jmprel_va;
    size_t    relac;
    size_t    jmprelc;
    uint32_t  sym_count;
    uint64_t  strsz;
    char      interp_path[ELF64_NAME_MAX];
    char      needed[ELF64_MAX_NEEDED][ELF64_NAME_MAX];
    uint32_t  needed_count;
    uint8_t   has_dynamic;
} elf_dyn_info_t;

typedef struct {
    char          path[ELF64_NAME_MAX];
    elf64_ehdr_t  ehdr;
    elf64_phdr_t  phdrs[ELF64_MAX_PHDRS];
    uintptr_t     bias;
    uintptr_t     entry;
    uintptr_t     image_lo;
    uintptr_t     image_hi;
    uintptr_t     phdr_va;
    elf_dyn_info_t dyn;
    uint8_t       is_main;
    uint8_t       is_interp;
} elf_object_t;

static char elf_last_error[96];

extern const uint8_t _binary_user_demo_elf_start[];
extern const uint8_t _binary_user_demo_elf_end[];
extern const uint8_t _binary_user_dynamic_demo_elf_start[];
extern const uint8_t _binary_user_dynamic_demo_elf_end[];

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

static int elf_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b);
}

static void elf_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0U;

    if (cap == 0U) return;
    while (src && src[i] != '\0' && i + 1U < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uintptr_t elf_align_down(uintptr_t v)
{
    return v & ~(PAGE_SIZE - 1ULL);
}

static uintptr_t elf_align_up(uintptr_t v)
{
    return (v + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static uint32_t elf_order_for_size(size_t size)
{
    uint32_t order = 0U;
    size_t   bytes = PAGE_SIZE;

    while (bytes < size) {
        bytes <<= 1U;
        order++;
    }
    return order;
}

static uint32_t elf64_r_sym(uint64_t info)
{
    return (uint32_t)(info >> 32);
}

static uint32_t elf64_r_type(uint64_t info)
{
    return (uint32_t)(info & 0xFFFFFFFFULL);
}

static uint8_t elf64_st_bind(uint8_t info)
{
    return (uint8_t)(info >> 4);
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

static void elf_fill_aux_random(uint8_t out[16])
{
    uint64_t seed0;
    uint64_t seed1;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(seed0));
    seed1 = seed0 ^ timer_now_ticks() ^ (uint64_t)(uintptr_t)out ^
            0xA5A55A5ADEADBEEFULL;

    for (uint32_t i = 0U; i < 8U; i++) {
        out[i] = (uint8_t)((seed0 >> (i * 8U)) & 0xFFU);
        out[8U + i] = (uint8_t)((seed1 >> (i * 8U)) & 0xFFU);
    }
}

static int elf_build_stack(mm_space_t *space,
                           const char *const *argv, uint64_t argc,
                           const char *const *envp, uint64_t envc,
                           const elf64_ehdr_t *ehdr,
                           const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                           uintptr_t bias, uintptr_t at_base,
                           elf_image_t *out)
{
    uintptr_t arg_ptrs[ELF_LOADER_MAX_ARGS];
    uintptr_t env_ptrs[ELF_LOADER_MAX_ENVP];
    uintptr_t sp = MMU_USER_STACK_TOP;
    uintptr_t phdr_va;
    uintptr_t argv_va;
    uintptr_t envp_va;
    uintptr_t auxv_va;
    uintptr_t random_va;
    uint8_t   random_bytes[16];
    elf64_auxv_t auxv[16];
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

    sp -= sizeof(random_bytes);
    random_va = sp;
    {
        void *dst = mmu_space_resolve_ptr(space, random_va, sizeof(random_bytes));
        if (!dst) {
            elf_set_error("AT_RANDOM fuori mapping");
            return -1;
        }
        elf_fill_aux_random(random_bytes);
        memcpy(dst, random_bytes, sizeof(random_bytes));
    }
    sp &= ~0xFULL;

    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHDR,   phdr_va };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHENT,  ehdr->e_phentsize };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PHNUM,  ehdr->e_phnum };
    if (at_base != 0ULL)
        auxv[auxc++] = (elf64_auxv_t){ ELF_AT_BASE, at_base };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_ENTRY,  ehdr->e_entry + bias };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_PAGESZ, (uint64_t)PAGE_SIZE };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_HWCAP,  ELF64_HWCAP };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_RANDOM, random_va };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_UID,    0ULL };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_EUID,   0ULL };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_GID,    0ULL };
    auxv[auxc++] = (elf64_auxv_t){ ELF_AT_EGID,   0ULL };
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

/*
 * elf_map_tls_block — alloca il blocco TLS statico per un ELF con PT_TLS.
 *
 * Bootstrap TLS AArch64 per il toolchain attuale:
 *   [TCB stub 16B] [TLS data (p_filesz)] [TLS bss/template tail]
 *
 * Il codice LE generato da GCC (`R_AARCH64_TLSLE_*`) indirizza le variabili
 * TLS con offset positivi da TPIDR_EL0, quindi il thread pointer deve puntare
 * a un piccolo TCB stub davanti al template PT_TLS.  Per la v1 statica ci
 * basta riservare 16 byte zeroed e copiare il template a partire da TP+16.
 *
 * Il blocco viene piazzato sotto lo stack, fuori dall'area di crescita.
 *
 * Ritorna 0 e scrive *tpidr_out con il valore iniziale di TPIDR_EL0.
 * Ritorna -1 in caso di errore.
 */
static int elf_map_tls_block(elf_read_fn rd, void *ctx,
                              mm_space_t *space,
                              const elf64_phdr_t *phdr,
                              uintptr_t bias,
                              uintptr_t *tpidr_out)
{
    const uintptr_t tcb_size = 16UL;
    /* Dimensioni e allineamento del template TLS */
    uintptr_t tls_align  = (phdr->p_align > 1UL) ? (uintptr_t)phdr->p_align : 1UL;
    uintptr_t tls_filesz = (uintptr_t)phdr->p_filesz;
    uintptr_t tls_memsz  = (uintptr_t)phdr->p_memsz;
    uintptr_t tls_data_off;
    uintptr_t tls_total_raw;
    uintptr_t tls_pages;
    uintptr_t tls_bytes;
    uintptr_t tp_va;
    uintptr_t tls_va;

    (void)bias;

    if (tls_align < tcb_size)
        tls_align = tcb_size;

    tls_data_off = (tcb_size + tls_align - 1UL) & ~(tls_align - 1UL);
    tls_total_raw = tls_data_off + tls_memsz;
    tls_pages = (tls_total_raw + PAGE_SIZE - 1UL) / PAGE_SIZE;
    tls_bytes = tls_pages * PAGE_SIZE;

    /*
     * Posizione nel VAS: sotto lo stack, con un gap di una pagina di guardia.
     * stack_base = MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE
     * TLS block  = stack_base - PAGE_SIZE (guardia) - tls_bytes
     */
    tp_va = (MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE)
            - PAGE_SIZE          /* pagina di guardia */
            - tls_bytes;
    tp_va &= ~(tls_align - 1UL);
    tls_va = tp_va + tls_data_off;

    if (mmu_map_user_region(space, tp_va, tls_bytes,
                             MMU_PROT_USER_R | MMU_PROT_USER_W) < 0) {
        elf_set_error("map TLS block fallita");
        return -1;
    }

    /* Azzera tutto il blocco (bss TLS inizia a zero) */
    void *dst = mmu_space_resolve_ptr(space, tp_va, tls_bytes);
    if (!dst) {
        elf_set_error("resolve TLS VA fallita");
        return -1;
    }
    memset(dst, 0, tls_bytes);

    /* Copia il template TLS (p_filesz byte dall'offset p_offset nel file) */
    if (tls_filesz > 0UL) {
        uintptr_t copied = 0UL;
        uintptr_t src_off = (uintptr_t)phdr->p_offset;
        while (copied < tls_filesz) {
            size_t chunk = 512U;
            if ((uintptr_t)chunk > tls_filesz - copied)
                chunk = (size_t)(tls_filesz - copied);
            void *d = mmu_space_resolve_ptr(space, tls_va + copied, chunk);
            if (!d) {
                elf_set_error("copy TLS template fuori mapping");
                return -1;
            }
            if (elf_read_exact(rd, ctx, src_off + copied, d, chunk) < 0) {
                elf_set_error("read TLS template fallita");
                return -1;
            }
            copied += chunk;
        }
    }

    /* TPIDR_EL0 punta al TCB stub; il template PT_TLS parte a TP+tls_data_off. */
    *tpidr_out = tp_va;
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
    if (mmu_space_map_signal_trampoline(space) < 0) {
        elf_set_error("map signal trampoline fallita");
        mmu_space_destroy(space);
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

    /* M11-01b: if the ELF has a PT_TLS segment, allocate the TLS block
     * and compute the initial thread pointer value. */
    for (uint32_t i = 0U; i < ehdr.e_phnum; i++) {
        uintptr_t tpidr = 0UL;
        if (phdrs[i].p_type != PT_TLS || phdrs[i].p_memsz == 0ULL)
            continue;
        if (elf_map_tls_block(rd, ctx, space, &phdrs[i], bias, &tpidr) < 0)
            goto fail;
        image.tpidr_el0 = tpidr;
        break;
    }

    if (elf_build_stack(space, argv, argc, envp, envc,
                        &ehdr, phdrs, bias, 0ULL, &image) < 0)
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

static int elf_copy_space_string(mm_space_t *space, uintptr_t va,
                                 char *dst, size_t cap)
{
    size_t i;

    if (!space || !dst || cap == 0U)
        return -1;

    for (i = 0U; i + 1U < cap; i++) {
        char *src = (char *)mmu_space_resolve_ptr(space, va + i, 1U);
        if (!src)
            return -1;
        dst[i] = *src;
        if (dst[i] == '\0')
            return 0;
    }

    dst[cap - 1U] = '\0';
    return -1;
}

static int elf_normalize_path(const char *name, char *dst, size_t cap)
{
    size_t i = 0U;
    size_t off = 0U;

    if (!name || !dst || cap < 2U)
        return -1;

    if (name[0] != '/') {
        dst[0] = '/';
        off = 1U;
    }

    while (name[i] != '\0' && off + i + 1U < cap) {
        dst[off + i] = name[i];
        i++;
    }
    if (name[i] != '\0')
        return -1;

    dst[off + i] = '\0';
    return 0;
}

static int elf_prefault_object(mm_space_t *space, const elf_object_t *obj)
{
    return elf_prefault_loaded_image(space, &obj->ehdr, obj->phdrs, obj->bias);
}

static int elf_plan_object(const elf64_ehdr_t *ehdr,
                           const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                           uintptr_t *next_dyn_base,
                           uintptr_t *bias_out,
                           uintptr_t *image_lo_out,
                           uintptr_t *image_hi_out)
{
    uintptr_t min_va = 0ULL;
    uintptr_t max_va = 0ULL;
    uintptr_t bias = 0ULL;
    uintptr_t stack_base = MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE;
    uint8_t   have_load = 0U;

    for (uint32_t i = 0U; i < ehdr->e_phnum; i++) {
        uintptr_t seg_lo;
        uintptr_t seg_hi;

        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0ULL)
            continue;

        seg_lo = elf_align_down((uintptr_t)phdrs[i].p_vaddr);
        seg_hi = elf_align_up((uintptr_t)(phdrs[i].p_vaddr + phdrs[i].p_memsz));
        if (!have_load || seg_lo < min_va) min_va = seg_lo;
        if (!have_load || seg_hi > max_va) max_va = seg_hi;
        have_load = 1U;
    }

    if (!have_load) {
        elf_set_error("ELF senza PT_LOAD");
        return -1;
    }

    if (ehdr->e_type == ET_DYN) {
        uintptr_t base = (*next_dyn_base < ELF64_DYN_BASE) ?
                         ELF64_DYN_BASE : *next_dyn_base;
        base = elf_align_up(base);
        bias = base - min_va;
    }

    if (min_va + bias < MMU_USER_BASE || max_va + bias > stack_base) {
        elf_set_error("oggetto ELF fuori window user");
        return -1;
    }

    if (ehdr->e_type == ET_DYN)
        *next_dyn_base = elf_align_up(max_va + bias + PAGE_SIZE);
    else if (max_va + bias >= *next_dyn_base)
        *next_dyn_base = elf_align_up(max_va + bias + PAGE_SIZE);

    *bias_out = bias;
    *image_lo_out = min_va + bias;
    *image_hi_out = max_va + bias;
    return 0;
}

static int elf_map_object_segments(elf_read_fn rd, void *ctx, size_t image_size,
                                   const elf64_ehdr_t *ehdr,
                                   const elf64_phdr_t phdrs[ELF64_MAX_PHDRS],
                                   mm_space_t *space, uintptr_t bias)
{
    for (uint32_t i = 0U; i < ehdr->e_phnum; i++) {
        uintptr_t seg_va;
        uintptr_t seg_lo;
        uintptr_t seg_hi;
        uint32_t  prot = 0U;
        uint64_t  copied = 0ULL;

        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0ULL)
            continue;
        if (phdrs[i].p_filesz > phdrs[i].p_memsz) {
            elf_set_error("p_filesz > p_memsz");
            return -1;
        }
        if (phdrs[i].p_offset > image_size ||
            phdrs[i].p_filesz > image_size - phdrs[i].p_offset) {
            elf_set_error("segmento fuori file");
            return -1;
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
            return -1;
        }

        while (copied < phdrs[i].p_filesz) {
            size_t chunk = 512U;
            void  *dst;

            if ((uint64_t)chunk > phdrs[i].p_filesz - copied)
                chunk = (size_t)(phdrs[i].p_filesz - copied);

            dst = mmu_space_resolve_ptr(space, seg_va + copied, chunk);
            if (!dst) {
                elf_set_error("copy PT_LOAD fuori mapping");
                return -1;
            }
            if (elf_read_exact(rd, ctx, phdrs[i].p_offset + copied, dst, chunk) < 0) {
                elf_set_error("read PT_LOAD fallita");
                return -1;
            }
            copied += chunk;
        }
    }

    return 0;
}

static int elf_parse_interp(mm_space_t *space, elf_object_t *obj)
{
    for (uint32_t i = 0U; i < obj->ehdr.e_phnum; i++) {
        uintptr_t interp_va;

        if (obj->phdrs[i].p_type != PT_INTERP || obj->phdrs[i].p_filesz == 0ULL)
            continue;

        interp_va = obj->bias + (uintptr_t)obj->phdrs[i].p_vaddr;
        if (elf_copy_space_string(space, interp_va,
                                  obj->dyn.interp_path,
                                  sizeof(obj->dyn.interp_path)) < 0) {
            elf_set_error("PT_INTERP non leggibile");
            return -1;
        }
        return 0;
    }

    return 0;
}

static int elf_parse_dynamic(mm_space_t *space, elf_object_t *obj)
{
    uintptr_t dyn_va = 0ULL;
    size_t    dyn_bytes = 0U;
    uint32_t  needed_offsets[ELF64_MAX_NEEDED];
    uint32_t  needed_count = 0U;
    uint8_t   have_pltrel = 0U;

    memset(&obj->dyn, 0, sizeof(obj->dyn));

    for (uint32_t i = 0U; i < obj->ehdr.e_phnum; i++) {
        if (obj->phdrs[i].p_type == PT_DYNAMIC) {
            dyn_va = obj->bias + (uintptr_t)obj->phdrs[i].p_vaddr;
            dyn_bytes = (size_t)obj->phdrs[i].p_memsz;
            break;
        }
    }

    if (dyn_va == 0ULL || dyn_bytes < sizeof(elf64_dyn_t))
        return 0;

    for (size_t i = 0U; i < dyn_bytes / sizeof(elf64_dyn_t); i++) {
        elf64_dyn_t *dyn = (elf64_dyn_t *)mmu_space_resolve_ptr(
            space, dyn_va + i * sizeof(elf64_dyn_t), sizeof(elf64_dyn_t));

        if (!dyn) {
            elf_set_error("dynamic section fuori mapping");
            return -1;
        }
        if (dyn->d_tag == (int64_t)DT_NULL)
            break;

        switch ((uint64_t)dyn->d_tag) {
        case DT_NEEDED:
            if (needed_count >= ELF64_MAX_NEEDED) {
                elf_set_error("troppe DT_NEEDED");
                return -1;
            }
            needed_offsets[needed_count++] = (uint32_t)dyn->d_val;
            break;
        case DT_HASH:
            obj->dyn.hash_va = obj->bias + (uintptr_t)dyn->d_val;
            break;
        case DT_STRTAB:
            obj->dyn.dynstr_va = obj->bias + (uintptr_t)dyn->d_val;
            break;
        case DT_SYMTAB:
            obj->dyn.dynsym_va = obj->bias + (uintptr_t)dyn->d_val;
            break;
        case DT_RELA:
            obj->dyn.rela_va = obj->bias + (uintptr_t)dyn->d_val;
            break;
        case DT_RELASZ:
            obj->dyn.relac = (size_t)(dyn->d_val / sizeof(elf64_rela_t));
            break;
        case DT_RELAENT:
            if (dyn->d_val != sizeof(elf64_rela_t)) {
                elf_set_error("RELAENT non supportato");
                return -1;
            }
            break;
        case DT_STRSZ:
            obj->dyn.strsz = dyn->d_val;
            break;
        case DT_SYMENT:
            if (dyn->d_val != sizeof(elf64_sym_t)) {
                elf_set_error("SYMENT non supportato");
                return -1;
            }
            break;
        case DT_JMPREL:
            obj->dyn.jmprel_va = obj->bias + (uintptr_t)dyn->d_val;
            break;
        case DT_PLTRELSZ:
            obj->dyn.jmprelc = (size_t)(dyn->d_val / sizeof(elf64_rela_t));
            break;
        case DT_PLTREL:
            have_pltrel = 1U;
            if (dyn->d_val != DT_RELA) {
                elf_set_error("PLTREL non-RELA non supportato");
                return -1;
            }
            break;
        case DT_PLTGOT:
        case DT_DEBUG:
            break;
        default:
            break;
        }
    }

    obj->dyn.has_dynamic = 1U;
    if (obj->dyn.dynsym_va == 0ULL || obj->dyn.dynstr_va == 0ULL ||
        obj->dyn.hash_va == 0ULL || obj->dyn.strsz == 0ULL) {
        elf_set_error("dynamic metadata incompleta");
        return -1;
    }
    if (obj->dyn.jmprelc != 0U && !have_pltrel) {
        elf_set_error("JMPREL senza PLTREL");
        return -1;
    }

    {
        uint32_t *hash = (uint32_t *)mmu_space_resolve_ptr(space, obj->dyn.hash_va,
                                                           2U * sizeof(uint32_t));
        if (!hash) {
            elf_set_error("DT_HASH non leggibile");
            return -1;
        }
        obj->dyn.sym_count = hash[1];
    }

    for (uint32_t i = 0U; i < needed_count; i++) {
        if ((uint64_t)needed_offsets[i] >= obj->dyn.strsz) {
            elf_set_error("DT_NEEDED fuori dynstr");
            return -1;
        }
        if (elf_copy_space_string(space,
                                  obj->dyn.dynstr_va + needed_offsets[i],
                                  obj->dyn.needed[i],
                                  sizeof(obj->dyn.needed[i])) < 0) {
            elf_set_error("DT_NEEDED non leggibile");
            return -1;
        }
    }
    obj->dyn.needed_count = needed_count;
    return 0;
}

static int elf_find_loaded_object(const elf_object_t objects[ELF64_MAX_OBJECTS],
                                  uint32_t object_count, const char *path)
{
    for (uint32_t i = 0U; i < object_count; i++) {
        if (elf_streq(objects[i].path, path))
            return (int)i;
    }
    return -1;
}

static int elf_load_vfs_object(mm_space_t *space, const char *path,
                               uintptr_t *next_dyn_base,
                               elf_object_t *obj)
{
    vfs_file_t file;
    stat_t     st;
    int        rc;
    uintptr_t  bias;
    uintptr_t  image_lo;
    uintptr_t  image_hi;

    if (vfs_open(path, O_RDONLY, &file) < 0) {
        elf_set_error("open oggetto ELF fallita");
        return -1;
    }

    rc = vfs_stat(&file, &st);
    if (rc < 0) {
        (void)vfs_close(&file);
        elf_set_error("stat oggetto ELF fallita");
        return -1;
    }

    memset(obj, 0, sizeof(*obj));
    elf_strlcpy(obj->path, path, sizeof(obj->path));

    if (elf_read_exact(elf_vfs_read, &file, 0ULL, &obj->ehdr, sizeof(obj->ehdr)) < 0) {
        (void)vfs_close(&file);
        elf_set_error("read ELF header fallita");
        return -1;
    }
    if (elf_validate_header(&obj->ehdr, (size_t)st.st_size) < 0) {
        (void)vfs_close(&file);
        return -1;
    }
    if (elf_read_exact(elf_vfs_read, &file, obj->ehdr.e_phoff, obj->phdrs,
                       (size_t)obj->ehdr.e_phnum * sizeof(elf64_phdr_t)) < 0) {
        (void)vfs_close(&file);
        elf_set_error("read program headers fallita");
        return -1;
    }
    if (elf_plan_object(&obj->ehdr, obj->phdrs, next_dyn_base,
                        &bias, &image_lo, &image_hi) < 0) {
        (void)vfs_close(&file);
        return -1;
    }
    if (elf_map_object_segments(elf_vfs_read, &file, (size_t)st.st_size,
                                &obj->ehdr, obj->phdrs, space, bias) < 0) {
        (void)vfs_close(&file);
        return -1;
    }
    (void)vfs_close(&file);

    obj->bias = bias;
    obj->entry = (uintptr_t)obj->ehdr.e_entry + bias;
    obj->image_lo = image_lo;
    obj->image_hi = image_hi;
    obj->phdr_va = elf_phdr_runtime_va(&obj->ehdr, obj->phdrs, bias);

    if (elf_parse_interp(space, obj) < 0)
        return -1;
    if (elf_parse_dynamic(space, obj) < 0)
        return -1;
    return 0;
}

static int elf_lookup_symbol_in_object(mm_space_t *space, const elf_object_t *obj,
                                       const char *name, uintptr_t *value_out)
{
    char sym_name[ELF64_NAME_MAX];

    if (!obj->dyn.has_dynamic || !name)
        return -1;

    for (uint32_t i = 0U; i < obj->dyn.sym_count; i++) {
        elf64_sym_t *sym = (elf64_sym_t *)mmu_space_resolve_ptr(
            space, obj->dyn.dynsym_va + (uintptr_t)i * sizeof(elf64_sym_t),
            sizeof(elf64_sym_t));

        if (!sym)
            return -1;
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        if ((uint64_t)sym->st_name >= obj->dyn.strsz)
            continue;
        if (elf_copy_space_string(space, obj->dyn.dynstr_va + sym->st_name,
                                  sym_name, sizeof(sym_name)) < 0)
            return -1;
        if (elf_streq(sym_name, name)) {
            *value_out = obj->bias + (uintptr_t)sym->st_value;
            return 0;
        }
    }

    return -1;
}

static int elf_resolve_symbol(mm_space_t *space,
                              const elf_object_t objects[ELF64_MAX_OBJECTS],
                              uint32_t object_count,
                              const elf_object_t *source,
                              uint32_t sym_index,
                              uintptr_t *value_out)
{
    elf64_sym_t *sym;
    char         name[ELF64_NAME_MAX];

    if (!source->dyn.has_dynamic || sym_index >= source->dyn.sym_count) {
        elf_set_error("sym index fuori range");
        return -1;
    }

    sym = (elf64_sym_t *)mmu_space_resolve_ptr(
        space, source->dyn.dynsym_va + (uintptr_t)sym_index * sizeof(elf64_sym_t),
        sizeof(elf64_sym_t));
    if (!sym) {
        elf_set_error("dynsym non leggibile");
        return -1;
    }

    if (sym->st_shndx != SHN_UNDEF) {
        *value_out = source->bias + (uintptr_t)sym->st_value;
        return 0;
    }
    if ((uint64_t)sym->st_name >= source->dyn.strsz) {
        elf_set_error("st_name fuori dynstr");
        return -1;
    }
    if (elf_copy_space_string(space, source->dyn.dynstr_va + sym->st_name,
                              name, sizeof(name)) < 0) {
        elf_set_error("nome simbolo non leggibile");
        return -1;
    }

    for (uint32_t i = 0U; i < object_count; i++) {
        if (&objects[i] == source)
            continue;
        if (elf_lookup_symbol_in_object(space, &objects[i], name, value_out) == 0)
            return 0;
    }

    if (elf64_st_bind(sym->st_info) == STB_WEAK) {
        *value_out = 0ULL;
        return 0;
    }

    elf_set_error("simbolo dinamico non risolto");
    return -1;
}

static int elf_apply_relocations(mm_space_t *space,
                                 const elf_object_t objects[ELF64_MAX_OBJECTS],
                                 uint32_t object_count,
                                 const elf_object_t *obj,
                                 uintptr_t rela_va, size_t relac)
{
    for (size_t i = 0U; i < relac; i++) {
        elf64_rela_t *rela = (elf64_rela_t *)mmu_space_resolve_ptr(
            space, rela_va + (uintptr_t)i * sizeof(elf64_rela_t),
            sizeof(elf64_rela_t));
        uint64_t *where;
        uintptr_t where_va;
        uintptr_t value;

        if (!rela) {
            elf_set_error("RELA non leggibile");
            return -1;
        }

        where_va = obj->bias + (uintptr_t)rela->r_offset;
        where = (uint64_t *)mmu_space_resolve_ptr(space, where_va, sizeof(uint64_t));
        if (!where) {
            elf_set_error("relocation target fuori mapping");
            return -1;
        }

        switch (elf64_r_type(rela->r_info)) {
        case R_AARCH64_RELATIVE:
            *where = obj->bias + (uintptr_t)rela->r_addend;
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            if (elf_resolve_symbol(space, objects, object_count, obj,
                                   elf64_r_sym(rela->r_info), &value) < 0)
                return -1;
            *where = value + (uintptr_t)rela->r_addend;
            break;
        default:
            elf_set_error("relocation AArch64 non supportata");
            return -1;
        }
    }

    return 0;
}

static int elf_apply_object_dynamic(mm_space_t *space,
                                    const elf_object_t objects[ELF64_MAX_OBJECTS],
                                    uint32_t object_count,
                                    const elf_object_t *obj)
{
    if (!obj->dyn.has_dynamic)
        return 0;

    if (obj->dyn.relac != 0U &&
        elf_apply_relocations(space, objects, object_count, obj,
                              obj->dyn.rela_va, obj->dyn.relac) < 0)
        return -1;

    if (obj->dyn.jmprelc != 0U &&
        (obj->dyn.jmprel_va != obj->dyn.rela_va || obj->dyn.jmprelc != obj->dyn.relac) &&
        elf_apply_relocations(space, objects, object_count, obj,
                              obj->dyn.jmprel_va, obj->dyn.jmprelc) < 0)
        return -1;

    return 0;
}

static int elf_load_path_exec_dynamic(const char *path,
                                      const char *const *argv, uint64_t argc,
                                      const char *const *envp, uint64_t envc,
                                      elf_image_t *out)
{
    elf_image_t  image;
    elf_object_t *objects;
    mm_space_t  *space;
    uintptr_t    next_dyn_base = ELF64_DYN_BASE;
    uintptr_t    max_hi = 0ULL;
    uint32_t     object_count = 0U;
    const char  *fallback_argv[1];
    int          interp_idx = -1;
    uint64_t     objects_pa = 0ULL;
    uint32_t     objects_order = 0U;

    if (!path || path[0] == '\0') {
        elf_set_error("path ELF vuoto");
        return -1;
    }
    if (!out) {
        elf_set_error("output ELF nullo");
        return -1;
    }
    if (!argv || argc == 0ULL) {
        fallback_argv[0] = path;
        argv = fallback_argv;
        argc = 1ULL;
    }

    memset(&image, 0, sizeof(image));
    objects_order = elf_order_for_size(sizeof(elf_object_t) * ELF64_MAX_OBJECTS);
    objects_pa = phys_alloc_pages(objects_order);
    if (objects_pa == 0ULL) {
        elf_set_error("buffer oggetti ELF esaurito");
        return -1;
    }
    objects = (elf_object_t *)(uintptr_t)objects_pa;
    memset(objects, 0, PAGE_SIZE << objects_order);

    space = mmu_space_create();
    if (!space) {
        elf_set_error("mm_space esauriti");
        goto fail_free_objects;
    }
    if (mmu_space_map_signal_trampoline(space) < 0) {
        elf_set_error("map signal trampoline fallita");
        goto fail;
    }

    if (elf_load_vfs_object(space, path, &next_dyn_base, &objects[0]) < 0)
        goto fail;
    objects[0].is_main = 1U;
    object_count = 1U;
    max_hi = objects[0].image_hi;

    if (objects[0].dyn.interp_path[0] != '\0') {
        char interp_path[ELF64_NAME_MAX];

        if (elf_normalize_path(objects[0].dyn.interp_path,
                               interp_path, sizeof(interp_path)) < 0) {
            elf_set_error("PT_INTERP troppo lungo");
            goto fail;
        }
        if (elf_find_loaded_object(objects, object_count, interp_path) < 0) {
            if (object_count >= ELF64_MAX_OBJECTS) {
                elf_set_error("troppi oggetti ELF");
                goto fail;
            }
            if (elf_load_vfs_object(space, interp_path, &next_dyn_base,
                                    &objects[object_count]) < 0)
                goto fail;
            objects[object_count].is_interp = 1U;
            interp_idx = (int)object_count;
            if (objects[object_count].image_hi > max_hi)
                max_hi = objects[object_count].image_hi;
            object_count++;
        }
    }

    for (uint32_t idx = 0U; idx < object_count; idx++) {
        for (uint32_t n = 0U; n < objects[idx].dyn.needed_count; n++) {
            char needed_path[ELF64_NAME_MAX];

            if (elf_normalize_path(objects[idx].dyn.needed[n],
                                   needed_path, sizeof(needed_path)) < 0) {
                elf_set_error("DT_NEEDED troppo lungo");
                goto fail;
            }
            if (elf_find_loaded_object(objects, object_count, needed_path) >= 0)
                continue;
            if (object_count >= ELF64_MAX_OBJECTS) {
                elf_set_error("troppi oggetti ELF");
                goto fail;
            }
            if (elf_load_vfs_object(space, needed_path, &next_dyn_base,
                                    &objects[object_count]) < 0)
                goto fail;
            if (objects[object_count].image_hi > max_hi)
                max_hi = objects[object_count].image_hi;
            object_count++;
        }
    }

    for (uint32_t i = 0U; i < object_count; i++) {
        if (elf_apply_object_dynamic(space, objects, object_count, &objects[i]) < 0)
            goto fail;
    }

    if (mmu_map_user_region(space,
                            MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE,
                            MMU_USER_STACK_SIZE,
                            MMU_PROT_USER_R | MMU_PROT_USER_W) < 0) {
        elf_set_error("map stack user fallita");
        goto fail;
    }

    image.space = space;
    image.entry = objects[0].entry;
    image.image_base = objects[0].image_lo;
    image.image_end = max_hi;
    image.phentsize = objects[0].ehdr.e_phentsize;
    image.phnum = objects[0].ehdr.e_phnum;

    /* M11-01b: PT_TLS from main executable (objects[0]).
     * Re-open the file to use the VFS reader for the TLS template copy. */
    for (uint32_t i = 0U; i < objects[0].ehdr.e_phnum; i++) {
        if (objects[0].phdrs[i].p_type != PT_TLS ||
            objects[0].phdrs[i].p_memsz == 0ULL) continue;
        {
            vfs_file_t tls_file;
            uintptr_t  tpidr = 0UL;
            if (vfs_open(objects[0].path, O_RDONLY, &tls_file) < 0) {
                elf_set_error("open TLS file fallita");
                goto fail;
            }
            int tls_rc = elf_map_tls_block(elf_vfs_read, &tls_file, space,
                                           &objects[0].phdrs[i], objects[0].bias,
                                           &tpidr);
            (void)vfs_close(&tls_file);
            if (tls_rc < 0) goto fail;
            image.tpidr_el0 = tpidr;
        }
        break;
    }

    if (elf_build_stack(space, argv, argc, envp, envc,
                        &objects[0].ehdr, objects[0].phdrs, objects[0].bias,
                        (interp_idx >= 0) ? objects[interp_idx].image_lo : 0ULL,
                        &image) < 0)
        goto fail;

    for (uint32_t i = 0U; i < object_count; i++) {
        if (elf_prefault_object(space, &objects[i]) < 0)
            goto fail;
    }

    *out = image;
    elf_set_error("OK");
    phys_free_pages(objects_pa, objects_order);
    return 0;

fail:
    mmu_space_destroy(space);
fail_free_objects:
    phys_free_pages(objects_pa, objects_order);
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
    const char *argv_local[1];
    static const char *const env_default[] = {
        "PATH=/bin:/usr/bin",
        "HOME=/home/user",
        "PWD=/",
        "SHELL=/bin/arksh",
        "TERM=vt100",
        "USER=user",
    };

    if (!path || path[0] == '\0') {
        elf_set_error("path ELF vuoto");
        return -1;
    }

    argv_local[0] = (argv0 && argv0[0] != '\0') ? argv0 : path;
    return elf_load_path_exec_dynamic(path, argv_local, 1ULL,
                                      env_default,
                                      sizeof(env_default) / sizeof(env_default[0]),
                                      out);
}

int elf64_load_from_path_exec(const char *path,
                              const char *const *argv, uint64_t argc,
                              const char *const *envp, uint64_t envc,
                              elf_image_t *out)
{
    return elf_load_path_exec_dynamic(path, argv, argc, envp, envc, out);
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

    sched_tcb_t *t = sched_task_create_user(task_name ? task_name : "user-elf",
                                             image->space,
                                             image->entry,
                                             image->user_sp,
                                             image->argc,
                                             image->argv,
                                             image->envp,
                                             image->auxv,
                                             priority);
    if (t && image->tpidr_el0 != 0UL)
        sched_task_set_tpidr(t, image->tpidr_el0);
    return t;
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
