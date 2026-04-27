/*
 * EnlilOS Microkernel - ELF64 loader (M6-01 / M6-03)
 *
 * Loader minimale per ELF64 AArch64:
 *   - valida header e program header
 *   - mappa i PT_LOAD in uno mm_space dedicato
 *   - prepara stack ABI (argc/argv/envp/auxv)
 *   - pre-faulta segmenti e stack
 *   - da M6-03 supporta anche PT_INTERP + DT_NEEDED con linker dinamico
 *     kernel-side per PIE/ET_DYN e librerie condivise semplici
 *
 * M6-01 NON esponeva ancora execve(): il loader era usato internamente da
 * selftest e boot console. M6-02 lo collega a execve(), mentre M6-03
 * aggiunge il profilo dinamico minimo per demo/shared object user-space.
 */

#ifndef ENLILOS_ELF_LOADER_H
#define ENLILOS_ELF_LOADER_H

#include "mmu.h"
#include "sched.h"
#include "types.h"

#define ELF_AT_NULL     0U
#define ELF_AT_PHDR     3U
#define ELF_AT_PHENT    4U
#define ELF_AT_PHNUM    5U
#define ELF_AT_BASE     7U
#define ELF_AT_PAGESZ   6U
#define ELF_AT_ENTRY    9U
#define ELF_AT_HWCAP    16U
/* M11-01b: aggiuntivi per musl bootstrap */
#define ELF_AT_UID      11U
#define ELF_AT_EUID     12U
#define ELF_AT_GID      13U
#define ELF_AT_EGID     14U
#define ELF_AT_RANDOM   25U

#define ELF_LOADER_MAX_ARGS   16U
#define ELF_LOADER_MAX_ENVP   16U

#define ELF_RTLD_LAZY         0x0001U
#define ELF_RTLD_NOW          0x0002U
#define ELF_RTLD_LOCAL        0x0000U
#define ELF_RTLD_GLOBAL       0x0100U

typedef struct {
    mm_space_t *space;
    uintptr_t   entry;
    uintptr_t   phdr;
    uintptr_t   user_sp;
    uintptr_t   argv;
    uintptr_t   envp;
    uintptr_t   auxv;
    uintptr_t   image_base;
    uintptr_t   image_end;
    uintptr_t   stack_base;
    uintptr_t   stack_top;
    uintptr_t   tpidr_el0;  /* initial thread pointer (0 if no PT_TLS) — M11-01b */
    char        bundle_root[64];
    char        bundle_lib_path[64];
    uint64_t    argc;
    uint16_t    phentsize;
    uint16_t    phnum;
    uint8_t     abi_mode;
    uint8_t     _pad[3];
} elf_image_t;

int           elf64_load_from_memory(const void *image, size_t image_size,
                                     const char *argv0, elf_image_t *out);
int           elf64_load_from_path(const char *path, const char *argv0,
                                   elf_image_t *out);
int           elf64_load_from_path_exec(const char *path,
                                        const char *const *argv, uint64_t argc,
                                        const char *const *envp, uint64_t envc,
                                        elf_image_t *out);
void          elf64_unload_image(elf_image_t *image);

sched_tcb_t  *elf64_spawn_image(const char *task_name, const elf_image_t *image,
                                uint8_t priority);
int           elf64_spawn_path(const char *path, const char *argv0,
                               uint8_t priority, uint32_t *pid_out);
int           elf64_spawn_path_argv(const char *path,
                                    const char *const *argv, uint64_t argc,
                                    uint8_t priority, uint32_t *pid_out);
int           elf64_spawn_demo(uint8_t priority, uint32_t *pid_out);
int           elf64_dlopen_current(const char *path, uint32_t flags,
                                   uintptr_t *handle_out);
int           elf64_dlsym_current(uintptr_t handle, const char *name,
                                  uintptr_t *value_out);
int           elf64_dlclose_current(uintptr_t handle);
int           elf64_dlerror_drain_current(char *dst, size_t cap);
void          elf64_dlreset_proc(uint32_t proc_slot);

const char   *elf64_last_error(void);

#endif /* ENLILOS_ELF_LOADER_H */
