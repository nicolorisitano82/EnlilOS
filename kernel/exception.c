/*
 * NROS Microkernel - Exception Handler (M1-01)
 *
 * Decodifica ESR_EL1, stampa il contesto su UART e halt per eccezioni fatali.
 * Gli IRQ vengono separati per l'estensione al GIC-400 (M2-01).
 */

#include "exception.h"
#include "uart.h"

/* ── Helper: stampa uint64_t in hex su UART ─────────────────────────── */
static void print_hex(uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(hex[(val >> shift) & 0xF]);
    }
}

static void print_hex32(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hex[(val >> shift) & 0xF]);
    }
}

/* ── Decodifica EC (Exception Class) ────────────────────────────────── */
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

/* ── Dump del frame dei registri ─────────────────────────────────────── */
static void dump_registers(const exception_frame_t *f)
{
    uart_puts("\n  Registri generali:\n");
    for (int i = 0; i < 31; i += 2) {
        uart_puts("  x");
        uart_putc('0' + (i / 10));
        uart_putc('0' + (i % 10));
        uart_puts(" = ");
        print_hex(f->x[i]);
        if (i + 1 < 31) {
            uart_puts("    x");
            uart_putc('0' + ((i+1) / 10));
            uart_putc('0' + ((i+1) % 10));
            uart_puts(" = ");
            print_hex(f->x[i+1]);
        }
        uart_puts("\n");
    }
    uart_puts("\n  Registri speciali:\n");
    uart_puts("  PC   (ELR_EL1)  = "); print_hex(f->pc);   uart_puts("\n");
    uart_puts("  SP   (SP_EL0)   = "); print_hex(f->sp);   uart_puts("\n");
    uart_puts("  PSTATE (SPSR)   = "); print_hex(f->spsr); uart_puts("\n");
    uart_puts("  ESR_EL1         = "); print_hex32((uint32_t)f->esr); uart_puts("\n");
    uart_puts("  FAR_EL1         = "); print_hex(f->far);  uart_puts("\n");
}

/* ── Decodifica extra per Data/Instruction Abort ─────────────────────── */
static void decode_abort(uint64_t esr)
{
    uint32_t iss  = ESR_ISS(esr);
    uint32_t dfsc = iss & 0x3F;     /* Data/Instruction Fault Status Code */
    uint32_t wnr  = (iss >> 6) & 1; /* Write Not Read (solo Data Abort) */

    uart_puts("  Operazione  : ");
    uart_puts(wnr ? "WRITE\n" : "READ\n");

    uart_puts("  Fault Status: ");
    switch (dfsc & 0x3C) {
    case 0x04: uart_puts("Translation fault"); break;
    case 0x08: uart_puts("Access flag fault"); break;
    case 0x0C: uart_puts("Permission fault");  break;
    case 0x10: uart_puts("Sync External abort"); break;
    case 0x30: uart_puts("TLB conflict abort"); break;
    default:   uart_puts("Other/Reserved");    break;
    }
    uart_puts(" (level ");
    uart_putc('0' + (dfsc & 0x3));
    uart_puts(")\n");
}

/* ── Handler principale ──────────────────────────────────────────────── */
void exception_handler(exc_source_t src, exc_type_t type,
                       exception_frame_t *frame)
{
    uint32_t ec = ESR_EC(frame->esr);

    /* IRQ: percorso veloce, gestito separatamente */
    if (type == EXC_TYPE_IRQ) {
        irq_handler(src, frame);
        return;
    }

    /* FIQ: non usati, ignora */
    if (type == EXC_TYPE_FIQ) {
        return;
    }

    /* SVC da lower EL: punto di ingresso syscall (esteso in M3) */
    if (type == EXC_TYPE_SYNC && src == EXC_SRC_LOWER64 && ec == EC_SVC_AA64) {
        uint32_t svc_nr = (uint32_t)ESR_ISS(frame->esr);
        uart_puts("[NROS] SVC #");
        uart_putc('0' + svc_nr % 10);
        uart_puts(" (syscall - non ancora implementata)\n");
        /* Imposta ENOSYS in x0, continua */
        frame->x[0] = (uint64_t)-38; /* ENOSYS */
        return;
    }

    /* Tutte le altre eccezioni: dump e halt */
    uart_puts("\n");
    uart_puts("╔══════════════════════════════════════════════════╗\n");
    uart_puts("║          NROS KERNEL EXCEPTION                  ║\n");
    uart_puts("╚══════════════════════════════════════════════════╝\n");

    uart_puts("\n  Sorgente  : "); uart_puts(src_to_string(src));  uart_puts("\n");
    uart_puts("  Tipo      : "); uart_puts(type_to_string(type)); uart_puts("\n");
    uart_puts("  EC        : "); print_hex32(ec);
    uart_puts("  ("); uart_puts(ec_to_string(ec)); uart_puts(")\n");
    uart_puts("  ISS       : "); print_hex32(ESR_ISS(frame->esr)); uart_puts("\n");

    if (ec == EC_DABT_LOWER || ec == EC_DABT_SAME) {
        uart_puts("\n  [Data Abort]\n");
        uart_puts("  Indirizzo : "); print_hex(frame->far); uart_puts("\n");
        decode_abort(frame->esr);
    } else if (ec == EC_IABT_LOWER || ec == EC_IABT_SAME) {
        uart_puts("\n  [Instruction Abort]\n");
        uart_puts("  Indirizzo : "); print_hex(frame->far); uart_puts("\n");
        decode_abort(frame->esr);
    } else if (ec == EC_SP_ALIGN || ec == EC_PC_ALIGN) {
        uart_puts("\n  [Alignment Fault]\n");
        uart_puts("  Indirizzo : "); print_hex(frame->far); uart_puts("\n");
    }

    dump_registers(frame);

    uart_puts("\n[NROS] Eccezione fatale — sistema in halt\n\n");

    /* Halt: loop infinito con WFE */
    while (1) {
        __asm__ volatile("wfe");
    }
}

/* ── IRQ Handler ─────────────────────────────────────────────────────── */
/*
 * Chiamato da exception_handler() per ogni IRQ ricevuto.
 * Delega a gic_handle_irq() che esegue IAR→dispatch→EOIR in O(1).
 *
 * RT: questo percorso NON deve allocare memoria, NON deve acquisire lock
 * non-preemptibili, NON deve stampare su UART (tranne in handler specifici).
 */
#include "gic.h"

void irq_handler(exc_source_t src, exception_frame_t *frame)
{
    (void)src;
    (void)frame;
    gic_handle_irq();
}

/* ── Inizializzazione ────────────────────────────────────────────────── */
/*
 * exception_init() è chiamata dal boot prima di abilitare gli interrupt.
 * Imposta VBAR_EL1 per puntare alla tabella vettori in vectors.S.
 */
extern void exception_vectors(void); /* simbolo definito in vectors.S */

void exception_init(void)
{
    __asm__ volatile(
        "adr  x0, exception_vectors \n"
        "msr  vbar_el1, x0          \n"
        "isb                        \n"
        ::: "x0"
    );
    uart_puts("[NROS] Exception vectors installati\n");
}
