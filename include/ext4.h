/*
 * EnlilOS Microkernel - ext4 backend core (M5-03 / M5-04)
 *
 * Backend singleton per il primo mount ext4 su virtio-blk.
 * Espone un'interfaccia VFS con read path completo e write core per
 * file regolari gia' allocati: write, truncate e sync esplicito.
 */

#ifndef ENLILOS_EXT4_H
#define ENLILOS_EXT4_H

#include "vfs.h"

int             ext4_mount(void);
void            ext4_unmount(void);
int             ext4_is_mounted(void);
const char     *ext4_status(void);
const char     *ext4_label(void);
int             ext4_sync(void);
int             ext4_has_dirty(void);
int             ext4_service_writeback(uint64_t min_age_ms);
int             ext4_selftest_recovery(void);
const vfs_ops_t *ext4_vfs_ops(void);

#endif /* ENLILOS_EXT4_H */
