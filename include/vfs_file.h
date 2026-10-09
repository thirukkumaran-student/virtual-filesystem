#ifndef VFS_FILE_H
#define VFS_FILE_H

#include "vfs_disk.h"
#include "vfs_inode.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Maximum file size supported by the direct-block implementation.
 *
 * Eight direct pointers, each addressing one 4096-byte block.
 */
#define VFS_FILE_MAX_SIZE \
    ((uint64_t)VFS_INODE_DIRECT_BLOCKS * VFS_BLOCK_SIZE)

/*
 * Create a regular file and add its name to a directory.
 *
 * The parent directory must already exist.
 * On success, writes the allocated inode number to inode_number.
 *
 * Returns 0 on success and -1 on error.
 */
int vfs_file_create(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name,
    uint32_t *inode_number
);

/*
 * Read up to count bytes from a file, starting at offset.
 *
 * Returns the number of bytes read, or -1 on error.
 * Returns 0 when offset is at or beyond end-of-file.
 */
int64_t vfs_file_read(
    vfs_disk_t *disk,
    uint32_t inode_number,
    void *buffer,
    size_t count,
    uint64_t offset
);

/*
 * Write count bytes to a file, starting at offset.
 *
 * Returns the number of bytes written, or -1 on error.
 * This implementation does not support sparse writes.
 */
int64_t vfs_file_write(
    vfs_disk_t *disk,
    uint32_t inode_number,
    const void *buffer,
    size_t count,
    uint64_t offset
);

/*
 * Truncate a file to new_size bytes.
 *
 * Shrinking a file releases blocks that are no longer needed.
 * Growing a file is supported only up to the direct-block limit.
 */
int vfs_file_truncate(
    vfs_disk_t *disk,
    uint32_t inode_number,
    uint64_t new_size
);

#endif