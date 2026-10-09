
#ifndef VFS_CHECK_H
#define VFS_CHECK_H

#include "vfs_disk.h"

#include <stdio.h>

/*
 * Check filesystem metadata consistency.
 *
 * Returns:
 *   0  if all implemented checks pass
 *   1  if consistency problems are detected
 *  -1  if a disk I/O or operational error occurs
 */
int vfs_check(vfs_disk_t *disk, FILE *output);

#endif
