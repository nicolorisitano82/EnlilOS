/*
 * EnlilOS Microkernel - Kernel Self-Test Suite
 *
 * Suite minimale per validare i sottosistemi principali al boot o da target
 * dedicato `make test`.
 */

#ifndef ENLILOS_SELFTEST_H
#define ENLILOS_SELFTEST_H

#include "types.h"

int selftest_run_all(void);

#endif /* ENLILOS_SELFTEST_H */
