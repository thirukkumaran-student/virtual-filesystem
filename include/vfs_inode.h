#ifndef VFS_INODE_H
#define VFS_INODE_H

#include "vfs_disk.h"

#include <stdint.h>

#define VFS_INODE_DIRECT_BLOCKS 8U
#define VFS_INODE_RESERVED_SIZE 48U

/*
 * In-memory representation of a filesystem inode.
 *
 * The on-disk representation is exactly 128 bytes.
 * Do not write this C structure directly to disk.
 */
typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t links;

    uint64_t size;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    uint32_t flags;

    uint32_t direct_blocks[VFS_INODE_DIRECT_BLOCKS];
    uint32_t indirect_block;

    uint8_t reserved[VFS_INODE_RESERVED_SIZE];
} vfs_inode_t;

/*
 * Read an allocated inode from the inode table.
 *
 * Returns:
 *   0  on success
 *  -1  on error, with errno set
 */
int vfs_inode_read(
    vfs_disk_t *disk,
    uint32_t inode_number,
    vfs_inode_t *inode
);

/*
 * Write an allocated inode to the inode table.
 *
 * Returns:
 *   0  on success
 *  -1  on error, with errno set
 */
int vfs_inode_write(
    vfs_disk_t *disk,
    uint32_t inode_number,
    const vfs_inode_t *inode
);

#endif