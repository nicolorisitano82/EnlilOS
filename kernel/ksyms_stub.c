/*
 * EnlilOS Microkernel - fallback symbol table for pass1 link only.
 *
 * Il link finale usa kernel/ksyms_data.o generato a build-time. Tenere lo
 * stub in una TU separata evita che i riferimenti interni a kdebug.c vengano
 * risolti localmente sulle definizioni weak.
 */

#include "kdebug.h"

__attribute__((weak)) const ksym_entry_t kdebug_ksymtab[] = {
    { 0ULL, NULL }
};

__attribute__((weak)) const uint32_t kdebug_ksymtab_count = 0U;
