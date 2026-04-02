/*
 * EnlilOS Microkernel - Crash Reporter / Kernel Debugger (M14-02)
 */

#include "kdebug.h"
#include "gic.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"

#define KDEBUG_WATCHDOG_MS       10ULL
#define KDEBUG_WATCHDOG_SAMPLE_MS 5ULL
#define KDEBUG_BACKTRACE_DEPTH   16U
#define KDEBUG_PKT_MAX           64U

static uint64_t          watchdog_limit_cycles;
static uint64_t          watchdog_sample_cycles;
static volatile uint64_t watchdog_tick_epoch;
static volatile uint64_t watchdog_observed_epoch;
static volatile uint64_t watchdog_stalled_cycles;
static volatile uint32_t watchdog_armed;
static volatile uint32_t watchdog_pause_depth;
static volatile uint32_t panic_active;

static void pr_hex64(uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";

    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(hex[(val >> shift) & 0xFU]);
}

static void pr_hex32(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";

    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc(hex[(val >> shift) & 0xFU]);
}

static void pr_dec_u32(uint32_t v)
{
    char buf[12];
    int  len = 0;

    if (v == 0U) {
        uart_putc('0');
        return;
    }

    while (v != 0U) {
        buf[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (len > 0)
        uart_putc(buf[--len]);
}

static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a != '\0' && *b != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void kdebug_disable_irqs(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static const char *ec_to_string(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN:      return "Unknown/Uncategorized";
    case EC_SVC_AA64:     return "SVC (syscall) AArch64";
    case EC_IABT_LOWER:   return "Instruction Abort (lower EL)";
    case EC_IABT_SAME:    return "Instruction Abort (same EL)";
    case EC_PC_ALIGN:     return "PC Alignment Fault";
    case EC_DABT_LOWER:   return "Data Abort (lower EL)";
    case EC_DABT_SAME:    return "Data Abort (same EL)";
    case EC_SP_ALIGN:     return "SP Alignment Fault";
    case EC_FP_AA64:      return "FP/SIMD Exception AArch64";
    case EC_SERROR:       return "SError Interrupt";
    case EC_BRK_AA64:     return "BRK Instruction AArch64";
    default:              return "Reserved/Unknown";
    }
}

static const char *src_to_string(exc_source_t src)
{
    switch (src) {
    case EXC_SRC_SP_EL0:  return "Current EL / SP_EL0";
    case EXC_SRC_SP_ELX:  return "Current EL / SP_ELx";
    case EXC_SRC_LOWER64: return "Lower EL / AArch64";
    case EXC_SRC_LOWER32: return "Lower EL / AArch32";
    default:              return "Unknown";
    }
}

static const char *type_to_string(exc_type_t type)
{
    switch (type) {
    case EXC_TYPE_SYNC:   return "Synchronous";
    case EXC_TYPE_IRQ:    return "IRQ";
    case EXC_TYPE_FIQ:    return "FIQ";
    case EXC_TYPE_SERROR: return "SError";
    default:              return "Unknown";
    }
}

static void decode_abort(uint64_t esr)
{
    uint32_t iss  = ESR_ISS(esr);
    uint32_t dfsc = iss & 0x3FU;
    uint32_t wnr  = (iss >> 6) & 1U;

    uart_puts("  Operazione  : ");
    uart_puts(wnr ? "WRITE\n" : "READ\n");

    uart_puts("  Fault Status: ");
    switch (dfsc & 0x3CU) {
    case 0x04: uart_puts("Translation fault"); break;
    case 0x08: uart_puts("Access flag fault"); break;
    case 0x0C: uart_puts("Permission fault"); break;
    case 0x10: uart_puts("Sync External abort"); break;
    case 0x30: uart_puts("TLB conflict abort"); break;
    default:   uart_puts("Other/Reserved"); break;
    }
    uart_puts(" (level ");
    uart_putc((char)('0' + (dfsc & 0x3U)));
    uart_puts(")\n");
}

static void dump_registers(const exception_frame_t *f)
{
    uart_puts("\n  Registri generali:\n");
    for (int i = 0; i < 31; i += 2) {
        uart_puts("  x");
        uart_putc((char)('0' + (i / 10)));
        uart_putc((char)('0' + (i % 10)));
        uart_puts(" = ");
        pr_hex64(f->x[i]);
        if (i + 1 < 31) {
            uart_puts("    x");
            uart_putc((char)('0' + ((i + 1) / 10)));
            uart_putc((char)('0' + ((i + 1) % 10)));
            uart_puts(" = ");
            pr_hex64(f->x[i + 1]);
        }
        uart_puts("\n");
    }

    uart_puts("\n  Registri speciali:\n");
    uart_puts("  PC   (ELR_EL1)  = "); pr_hex64(f->pc);   uart_puts("\n");
    uart_puts("  SP   (SP_EL0)   = "); pr_hex64(f->sp);   uart_puts("\n");
    uart_puts("  PSTATE (SPSR)   = "); pr_hex64(f->spsr); uart_puts("\n");
    uart_puts("  ESR_EL1         = "); pr_hex32((uint32_t)f->esr); uart_puts("\n");
    uart_puts("  FAR_EL1         = "); pr_hex64(f->far);  uart_puts("\n");
}

static void dump_current_task(void)
{
    if (!current_task)
        return;

    uart_puts("\n  Task corrente:\n");
    uart_puts("  PID        : ");
    pr_dec_u32(current_task->pid);
    uart_puts("\n  Nome       : ");
    uart_puts(current_task->name ? current_task->name : "(unnamed)");
    uart_puts("\n  Priorita'  : ");
    pr_dec_u32(sched_task_effective_priority(current_task));
    uart_puts("\n  Stato      : ");
    pr_dec_u32(current_task->state);
    uart_puts("\n");
}

static int valid_fp(uint64_t fp)
{
    if ((fp & 0xFUL) != 0ULL)
        return 0;
    if (fp < PMM_BASE || fp + 16ULL > PMM_END)
        return 0;
    return 1;
}

int kdebug_lookup_symbol(uint64_t addr, const char **name_out, uint64_t *sym_addr_out)
{
    uint32_t lo = 0U;
    uint32_t hi = kdebug_ksymtab_count;
    uint32_t best = 0U;
    int      found = 0;

    if (name_out) *name_out = NULL;
    if (sym_addr_out) *sym_addr_out = 0ULL;
    if (kdebug_ksymtab_count == 0U)
        return -1;

    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint64_t cur = kdebug_ksymtab[mid].addr;

        if (cur == addr) {
            best = mid;
            found = 1;
            break;
        }
        if (cur < addr) {
            best = mid;
            found = 1;
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }

    if (!found || !kdebug_ksymtab[best].name)
        return -1;

    if (name_out) *name_out = kdebug_ksymtab[best].name;
    if (sym_addr_out) *sym_addr_out = kdebug_ksymtab[best].addr;
    return 0;
}

static void print_symbol_addr(uint64_t addr)
{
    const char *name;
    uint64_t    sym_addr;

    if (kdebug_lookup_symbol(addr, &name, &sym_addr) == 0) {
        uart_puts(name);
        uart_puts("+");
        pr_hex64(addr - sym_addr);
        uart_puts(" [");
        pr_hex64(addr);
        uart_puts("]");
        return;
    }

    pr_hex64(addr);
}

void kdebug_dump_backtrace_from(uint64_t fp, uint64_t pc, uint64_t lr)
{
    uint32_t frame_no = 0U;

    uart_puts("\n  Stack trace:\n");
    uart_puts("  #0  ");
    print_symbol_addr(pc);
    uart_puts("\n");
    frame_no = 1U;

    if (lr != 0ULL && lr != pc) {
        uart_puts("  #1  ");
        print_symbol_addr((lr >= 4ULL) ? (lr - 4ULL) : lr);
        uart_puts("\n");
        frame_no = 2U;
    }

    while (frame_no < KDEBUG_BACKTRACE_DEPTH) {
        uint64_t *frame_ptr;
        uint64_t  next_fp;
        uint64_t  saved_lr;

        if (!valid_fp(fp))
            break;

        frame_ptr = (uint64_t *)(uintptr_t)fp;
        next_fp = frame_ptr[0];
        saved_lr = frame_ptr[1];
        if (saved_lr == 0ULL)
            break;

        uart_puts("  #");
        pr_dec_u32(frame_no);
        uart_puts("  ");
        print_symbol_addr((saved_lr >= 4ULL) ? (saved_lr - 4ULL) : saved_lr);
        uart_puts("\n");
        frame_no++;

        if (next_fp <= fp)
            break;
        fp = next_fp;
    }
}

static void halt_forever(void)
{
    while (1)
        __asm__ volatile("wfe");
}

static inline void write_cntv_tval(uint64_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline void write_cntv_ctl(uint64_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

void kdebug_note_primary_tick(uint64_t now_cycles)
{
    (void)now_cycles;
    watchdog_tick_epoch++;
}

void kdebug_watchdog_pause(void)
{
    watchdog_pause_depth++;
}

void kdebug_watchdog_resume(void)
{
    if (watchdog_pause_depth == 0U)
        return;

    watchdog_pause_depth--;
    if (watchdog_pause_depth == 0U) {
        watchdog_observed_epoch = watchdog_tick_epoch;
        watchdog_stalled_cycles = 0ULL;
    }
}

static void watchdog_irq_handler(uint32_t irq, void *data)
{
    uint64_t epoch;

    (void)irq;
    (void)data;

    write_cntv_tval(watchdog_sample_cycles);
    if (!watchdog_armed || panic_active)
        return;
    if (watchdog_pause_depth != 0U)
        return;

    epoch = watchdog_tick_epoch;
    if (epoch != watchdog_observed_epoch) {
        watchdog_observed_epoch = epoch;
        watchdog_stalled_cycles = 0ULL;
        return;
    }

    watchdog_stalled_cycles += watchdog_sample_cycles;
    if (watchdog_stalled_cycles >= watchdog_limit_cycles) {
        kdebug_panic("watchdog timeout: tick bloccato oltre 10ms");
    }
}

void kdebug_init(void)
{
    watchdog_limit_cycles = timer_cntfrq() / 100ULL;
    if (watchdog_limit_cycles == 0ULL)
        watchdog_limit_cycles = 1ULL;
    watchdog_sample_cycles = timer_cntfrq() / 200ULL;
    if (watchdog_sample_cycles == 0ULL)
        watchdog_sample_cycles = 1ULL;

    watchdog_tick_epoch = 0ULL;
    watchdog_observed_epoch = 0ULL;
    watchdog_stalled_cycles = 0ULL;
    gic_register_irq(IRQ_TIMER_VIRT, watchdog_irq_handler,
                     NULL, GIC_PRIO_MAX, GIC_FLAG_EDGE);
    gic_enable_irq(IRQ_TIMER_VIRT);
    write_cntv_tval(watchdog_sample_cycles);
    write_cntv_ctl(CNTP_CTL_ENABLE);
    watchdog_armed = 1U;

    uart_puts("[KDEBUG] Crash reporter pronto: ksymtab=");
    pr_dec_u32(kdebug_ksymtab_count);
    uart_puts(" watchdog=");
    pr_dec_u32((uint32_t)KDEBUG_WATCHDOG_MS);
    uart_puts("ms\n");
}

int kdebug_watchdog_enabled(void)
{
    return watchdog_armed ? 1 : 0;
}

void kdebug_panic(const char *reason)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);

    kdebug_disable_irqs();
    if (panic_active) {
        uart_puts("\n[EnlilOS] PANIC ricorsivo — halt immediato\n");
        halt_forever();
    }
    panic_active = 1U;

    uart_puts("\n");
    uart_puts("╔══════════════════════════════════════════════════╗\n");
    uart_puts("║              EnlilOS KERNEL PANIC               ║\n");
    uart_puts("╚══════════════════════════════════════════════════╝\n");
    uart_puts("\n  Motivo    : ");
    uart_puts(reason ? reason : "(unknown)");
    uart_puts("\n");
    dump_current_task();
    kdebug_dump_backtrace_from((uint64_t)fp, (uint64_t)pc, (uint64_t)pc);
    uart_puts("\n[EnlilOS] Panic fatale — sistema in halt\n\n");
    halt_forever();
}

void kdebug_kassert_fail(const char *expr, const char *file, uint32_t line)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);

    kdebug_disable_irqs();
    if (panic_active) {
        uart_puts("\n[EnlilOS] KASSERT ricorsivo — halt immediato\n");
        halt_forever();
    }
    panic_active = 1U;

    uart_puts("\n");
    uart_puts("╔══════════════════════════════════════════════════╗\n");
    uart_puts("║              EnlilOS KASSERT FAILED             ║\n");
    uart_puts("╚══════════════════════════════════════════════════╝\n");
    uart_puts("\n  Espressione: ");
    uart_puts(expr ? expr : "(unknown)");
    uart_puts("\n  Posizione  : ");
    uart_puts(file ? file : "(unknown)");
    uart_puts(":");
    pr_dec_u32(line);
    uart_puts("\n");
    dump_current_task();
    kdebug_dump_backtrace_from((uint64_t)fp, (uint64_t)pc, (uint64_t)pc);
    uart_puts("\n[EnlilOS] Assert fatale — sistema in halt\n\n");
    halt_forever();
}

void kdebug_exception_report(exc_source_t src, exc_type_t type,
                             const exception_frame_t *frame)
{
    uint32_t ec;

    kdebug_disable_irqs();
    if (panic_active) {
        uart_puts("\n[EnlilOS] EXCEPTION ricorsiva — halt immediato\n");
        halt_forever();
    }
    panic_active = 1U;

    ec = ESR_EC(frame->esr);

    uart_puts("\n");
    uart_puts("╔══════════════════════════════════════════════════╗\n");
    uart_puts("║          EnlilOS KERNEL EXCEPTION               ║\n");
    uart_puts("╚══════════════════════════════════════════════════╝\n");

    uart_puts("\n  Sorgente  : "); uart_puts(src_to_string(src));   uart_puts("\n");
    uart_puts("  Tipo      : "); uart_puts(type_to_string(type)); uart_puts("\n");
    uart_puts("  EC        : "); pr_hex32(ec);
    uart_puts("  ("); uart_puts(ec_to_string(ec)); uart_puts(")\n");
    uart_puts("  ISS       : "); pr_hex32(ESR_ISS(frame->esr)); uart_puts("\n");

    if (ec == EC_DABT_LOWER || ec == EC_DABT_SAME) {
        uart_puts("\n  [Data Abort]\n");
        uart_puts("  Indirizzo : "); pr_hex64(frame->far); uart_puts("\n");
        decode_abort(frame->esr);
    } else if (ec == EC_IABT_LOWER || ec == EC_IABT_SAME) {
        uart_puts("\n  [Instruction Abort]\n");
        uart_puts("  Indirizzo : "); pr_hex64(frame->far); uart_puts("\n");
        decode_abort(frame->esr);
    } else if (ec == EC_SP_ALIGN || ec == EC_PC_ALIGN) {
        uart_puts("\n  [Alignment Fault]\n");
        uart_puts("  Indirizzo : "); pr_hex64(frame->far); uart_puts("\n");
    }

    dump_registers(frame);
    dump_current_task();
    kdebug_dump_backtrace_from(frame->x[29], frame->pc, frame->x[30]);

    uart_puts("\n[EnlilOS] Eccezione fatale — sistema in halt\n\n");
    halt_forever();
}

extern void kernel_main(void);
extern void sched_yield(void);
extern void exception_handler(exc_source_t src, exc_type_t type,
                              exception_frame_t *frame);

int kdebug_selftest_run(void)
{
    const char *name = NULL;
    uint64_t    sym_addr = 0ULL;

    if (kdebug_lookup_symbol((uint64_t)(uintptr_t)kernel_main, &name, &sym_addr) < 0)
        return -1;
    if (!streq(name, "kernel_main") || sym_addr != (uint64_t)(uintptr_t)kernel_main)
        return -1;

    if (kdebug_lookup_symbol((uint64_t)(uintptr_t)sched_yield, &name, &sym_addr) < 0)
        return -1;
    if (!streq(name, "sched_yield"))
        return -1;

    if (kdebug_lookup_symbol((uint64_t)(uintptr_t)exception_handler, &name, &sym_addr) < 0)
        return -1;
    if (!streq(name, "exception_handler"))
        return -1;

    if (!kdebug_watchdog_enabled())
        return -1;

    return 0;
}
