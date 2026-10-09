#ifndef VFS_ALLOC_H
#define VFS_ALLOC_H

#include "vfs_disk.h"

#include <stdint.h>

/*
 * Allocates the first available data block.
 *
 * The selected block is marked as allocated in the block bitmap,
 * and the superblock's free-block counter is decremented.
 *
 * Returns:
 *   0  on success
 *  -1  on failure; errno indicates the error
 */
int vfs_alloc_block(vfs_disk_t *disk, uint32_t *block_number);

/*
 * Frees an allocated data block.
 *
 * Metadata blocks and the root directory block cannot be freed.
 * The superblock's free-block counter is incremented.
 */
int vfs_free_block(vfs_disk_t *disk, uint32_t block_number);

/*
 * Allocates the first available inode.
 *
 * The inode is marked as allocated in the inode bitmap,
 * and the superblock's free-inode counter is decremented.
 */
int vfs_alloc_inode(vfs_disk_t *disk, uint32_t *inode_number);

/*
 * Frees an allocated non-root inode.
 *
 * The root inode cannot be freed.
 * The superblock's free-inode counter is incremented.
 */
int vfs_free_inode(vfs_disk_t *disk, uint32_t inode_number);

#endif