/*
 * EnlilOS Microkernel - ELF64 static loader (M6-01)
 *
 * Loader minimale per ELF64 AArch64 statici:
 *   - valida header e program header
 *   - mappa i PT_LOAD in uno mm_space dedicato
 *   - prepara stack ABI (argc/argv/envp/auxv)
 *   - pre-faulta segmenti e stack
 *
 * M6-01 NON espone ancora execve(): il loader e' usato internamente da
 * selftest e boot console. M6-02 colleghera' questa API alla syscall.
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
#define ELF_AT_PAGESZ   6U
#define ELF_AT_ENTRY    9U
#define ELF_AT_HWCAP    16U

#define ELF_LOADER_MAX_ARGS   16U
#define ELF_LOADER_MAX_ENVP   16U

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
    uint64_t    argc;
    uint16_t    phentsize;
    uint16_t    phnum;
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
int           elf64_spawn_demo(uint8_t priority, uint32_t *pid_out);

const char   *elf64_last_error(void);

#endif /* ENLILOS_ELF_LOADER_H */
