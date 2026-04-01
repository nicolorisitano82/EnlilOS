/*
 * EnlilOS Microkernel - ext4 read-only backend (M5-03)
 *
 * Backend singleton per il primo mount ext4 su virtio-blk.
 * Espone un'interfaccia VFS read-only: mount, open, read, readdir, stat.
 */

#ifndef ENLILOS_EXT4_H
#define ENLILOS_EXT4_H

#include "vfs.h"

int             ext4_mount(void);
void            ext4_unmount(void);
int             ext4_is_mounted(void);
const char     *ext4_status(void);
const char     *ext4_label(void);
const vfs_ops_t *ext4_vfs_ops(void);

#endif /* ENLILOS_EXT4_H */
