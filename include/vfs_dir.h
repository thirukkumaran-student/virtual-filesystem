#ifndef VFS_DIR_H
#define VFS_DIR_H

#include "vfs_disk.h"

#include <stdint.h>

#define VFS_DIRENT_REGULAR   1U
#define VFS_DIRENT_DIRECTORY 2U
#define VFS_NAME_MAX         255U

/*
 * Look up a filename in a directory.
 *
 * On success, writes the matching inode number to inode_number.
 * Returns 0 on success and -1 on error.
 */
int vfs_dir_lookup(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t *inode_number
);

/*
 * Add a directory entry for an already allocated inode.
 *
 * type must be VFS_DIRENT_REGULAR or VFS_DIRENT_DIRECTORY.
 * Duplicate names and invalid filenames are rejected.
 */
int vfs_dir_add(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t inode_number,
    uint8_t type
);

/*
 * Remove a directory entry by filename.
 *
 * The special entries "." and ".." cannot be removed.
 * This removes the directory entry only; inode reclamation
 * and link-count management belong to a higher-level operation.
 */
int vfs_dir_remove(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name
);

#endif