/*
 * NROS Microkernel - Exception Handling (M1-01)
 * AArch64 Exception Vector Table
 */

#ifndef NROS_EXCEPTION_H
#define NROS_EXCEPTION_H

#include "types.h"

/*
 * Frame salvato sullo stack dal gestore assembly.
 * Layout: 34 registri × 8 byte = 272 byte, allineato a 16 → 288 byte.
 *
 *  Offset  Registro
 *   0..240  x0..x30
 *    248    sp_el0
 *    256    elr_el1  (PC al momento dell'eccezione)
 *    264    spsr_el1 (PSTATE al momento dell'eccezione)
 *    272    esr_el1  (Exception Syndrome Register)
 *    280    far_el1  (Fault Address Register)
 */
typedef struct {
    uint64_t x[31];     /* x0 – x30                  */
    uint64_t sp;        /* SP_EL0                    */
    uint64_t pc;        /* ELR_EL1                   */
    uint64_t spsr;      /* SPSR_EL1                  */
    uint64_t esr;       /* ESR_EL1                   */
    uint64_t far;       /* FAR_EL1                   */
} exception_frame_t;

/* Dimensione frame: deve coincidere con il codice assembly */
#define EXCEPTION_FRAME_SIZE  288

/* ── ESR_EL1 campi ─────────────────────────────────────────────────── */

/* EC: Exception Class, bits [31:26] */
#define ESR_EC_SHIFT        26
#define ESR_EC_MASK         0x3F
#define ESR_EC(esr)         (((esr) >> ESR_EC_SHIFT) & ESR_EC_MASK)

/* IL: Instruction Length, bit 25 */
#define ESR_IL(esr)         (((esr) >> 25) & 1)

/* ISS: Instruction Specific Syndrome, bits [24:0] */
#define ESR_ISS(esr)        ((esr) & 0x01FFFFFF)

/* Exception Class values */
#define EC_UNKNOWN          0x00
#define EC_SVC_AA64         0x15    /* SVC da AArch64 (syscall)       */
#define EC_IABT_LOWER       0x20    /* Instruction Abort da EL inferiore */
#define EC_IABT_SAME        0x21    /* Instruction Abort da EL corrente  */
#define EC_PC_ALIGN         0x22    /* PC Alignment Fault               */
#define EC_DABT_LOWER       0x24    /* Data Abort da EL inferiore        */
#define EC_DABT_SAME        0x25    /* Data Abort da EL corrente         */
#define EC_SP_ALIGN         0x26    /* SP Alignment Fault               */
#define EC_FP_AA64          0x2C    /* FP exception da AArch64          */
#define EC_SERROR           0x2F    /* SError interrupt                 */
#define EC_BRK_AA64         0x3C    /* BRK instruction da AArch64       */

/* ── Sorgente dell'eccezione ────────────────────────────────────────── */
typedef enum {
    EXC_SRC_SP_EL0  = 0,   /* Current EL, SP_EL0        */
    EXC_SRC_SP_ELX  = 1,   /* Current EL, SP_ELx        */
    EXC_SRC_LOWER64 = 2,   /* Lower EL, AArch64         */
    EXC_SRC_LOWER32 = 3,   /* Lower EL, AArch32         */
} exc_source_t;

/* ── Tipo di eccezione ──────────────────────────────────────────────── */
typedef enum {
    EXC_TYPE_SYNC   = 0,
    EXC_TYPE_IRQ    = 1,
    EXC_TYPE_FIQ    = 2,
    EXC_TYPE_SERROR = 3,
} exc_type_t;

/* ── API pubblica ───────────────────────────────────────────────────── */

/* Inizializza e installa la vector table (imposta VBAR_EL1) */
void exception_init(void);

/* Handler C principale chiamato dall'assembly */
void exception_handler(exc_source_t src, exc_type_t type,
                       exception_frame_t *frame);

/* Handler IRQ (sarà esteso in M2) */
void irq_handler(exc_source_t src, exception_frame_t *frame);

#endif
