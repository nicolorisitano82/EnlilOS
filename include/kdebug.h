/*
 * EnlilOS Microkernel - Crash Reporter / Kernel Debugger (M14-02)
 *
 * Fornisce:
 *  - panic reporter con stack trace simbolico AArch64
 *  - KASSERT() RT-safe senza allocazioni dinamiche
 *  - watchdog secondario su CNTV (PPI #27)
 */

#ifndef ENLILOS_KDEBUG_H
#define ENLILOS_KDEBUG_H

#include "exception.h"
#include "types.h"

typedef struct {
    uint64_t    addr;
    const char *name;
} ksym_entry_t;

void kdebug_init(void);
void kdebug_note_primary_tick(uint64_t now_cycles);
void kdebug_watchdog_pause(void);
void kdebug_watchdog_resume(void);
void kdebug_panic(const char *reason);
void kdebug_kassert_fail(const char *expr, const char *file, uint32_t line);
void kdebug_exception_report(exc_source_t src, exc_type_t type,
                             const exception_frame_t *frame);
void kdebug_dump_backtrace_from(uint64_t fp, uint64_t pc, uint64_t lr);
int  kdebug_lookup_symbol(uint64_t addr, const char **name_out,
                          uint64_t *sym_addr_out);
int  kdebug_watchdog_enabled(void);
int  kdebug_selftest_run(void);

extern const ksym_entry_t kdebug_ksymtab[];
extern const uint32_t     kdebug_ksymtab_count;

#define KASSERT(cond) \
    do { \
        if (!(cond)) \
            kdebug_kassert_fail(#cond, __FILE__, (uint32_t)__LINE__); \
    } while (0)

#endif /* ENLILOS_KDEBUG_H */
