/*
 * EnlilOS Microkernel - Embedded initrd (M5-05)
 *
 * Root filesystem read-only basato su archivio CPIO "newc"
 * incorporato nel kernel image.
 */

#ifndef ENLILOS_INITRD_H
#define ENLILOS_INITRD_H

#include "vfs.h"

int              initrd_init(void);
int              initrd_is_ready(void);
const char      *initrd_status(void);
const vfs_ops_t *initrd_vfs_ops(void);

#endif /* ENLILOS_INITRD_H */
