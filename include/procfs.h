/*
 * EnlilOS Microkernel — procfs public header (M14-01)
 */

#ifndef ENLILOS_PROCFS_H
#define ENLILOS_PROCFS_H

#include "vfs.h"

const vfs_ops_t *procfs_vfs_ops(void);

#endif /* ENLILOS_PROCFS_H */
