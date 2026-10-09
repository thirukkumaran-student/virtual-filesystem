#define _POSIX_C_SOURCE 200809L

#include "vfs_alloc.h"
#include "vfs_format.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Superblock offsets defined by the filesystem format. */
enum {
    SB_FREE_BLOCKS_OFFSET = 64,
    SB_FREE_INODES_OFFSET = 68
};

/*
 * Read an unsigned 32-bit little-endian integer.
 */
static uint32_t get_u32_le(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16) |
        ((uint32_t)buffer[3] << 24);
}

/*
 * Write an unsigned 32-bit little-endian integer.
 */
static void put_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Validate the disk handle.
 */
static int validate_disk_handle(vfs_disk_t *disk)
{
    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

/*
 * Verify that the image has a valid superblock before modifying it.
 */
static int validate_formatted_disk(vfs_disk_t *disk)
{
    int result = vfs_is_formatted(disk);

    if (result == -1) {
        return -1;
    }

    if (result == 0) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/*
 * Test whether a bitmap bit is set.
 */
static bool bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8U] &
            (uint8_t)(1U << (bit % 8U))) != 0U;
}

/*
 * Set a bitmap bit.
 */
static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

/*
 * Clear a bitmap bit.
 */
static void bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8U] &= (uint8_t)~(1U << (bit % 8U));
}

/*
 * Allocate the first available data block.
 *
 * Block allocation begins at VFS_DATA_START. This prevents metadata
 * blocks and journal blocks from being returned to callers.
 *
 * This initial implementation assumes a single allocator at a time.
 * Thread-safe and crash-atomic allocation will require additional
 * synchronization and journaling.
 */
int vfs_alloc_block(vfs_disk_t *disk, uint32_t *block_number)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];
    uint8_t superblock[VFS_BLOCK_SIZE];

    uint32_t free_blocks;

    if (validate_disk_handle(disk) == -1) {
        return -1;
    }

    if (block_number == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_formatted_disk(disk) == -1) {
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    free_blocks = get_u32_le(
        superblock + SB_FREE_BLOCKS_OFFSET
    );

    if (free_blocks == 0U) {
        errno = ENOSPC;
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_BLOCK_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    /*
     * Search only the data region.
     */
    for (uint32_t block = VFS_DATA_START + 1U;
         block < VFS_TOTAL_BLOCKS;
         block++) {

        if (!bitmap_test(bitmap, block)) {
            /*
             * Mark the selected block as allocated.
             */
            bitmap_set(bitmap, block);

            /*
             * Persist the updated bitmap first.
             */
            if (vfs_disk_write_block(
                    disk,
                    VFS_BLOCK_BITMAP_BLOCK,
                    bitmap) == -1) {
                return -1;
            }

            /*
             * Update the superblock counter.
             */
            put_u32_le(
                superblock + SB_FREE_BLOCKS_OFFSET,
                free_blocks - 1U
            );

            if (vfs_disk_write_block(
                    disk,
                    VFS_SUPERBLOCK_BLOCK,
                    superblock) == -1) {
                /*
                 * The bitmap may now be updated while the counter
                 * is stale. The journal will address crash-atomic
                 * metadata updates in a later milestone.
                 */
                return -1;
            }

            *block_number = block;

            return 0;
        }
    }

    /*
     * The bitmap and superblock counter disagree if free_blocks
     * was nonzero but no free data block was found.
     */
    errno = EUCLEAN;
    return -1;
}

/*
 * Free an allocated data block.
 */
int vfs_free_block(vfs_disk_t *disk, uint32_t block_number)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];
    uint8_t superblock[VFS_BLOCK_SIZE];

    uint32_t free_blocks;

    if (validate_disk_handle(disk) == -1) {
        return -1;
    }

    if (block_number <= VFS_DATA_START ||
        block_number >= VFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    if (validate_formatted_disk(disk) == -1) {
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_BLOCK_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    if (!bitmap_test(bitmap, block_number)) {
        errno = EALREADY;
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    free_blocks = get_u32_le(
        superblock + SB_FREE_BLOCKS_OFFSET
    );

    if (free_blocks >= VFS_DATA_BLOCKS - 1U) {
        errno = EUCLEAN;
        return -1;
    }

    bitmap_clear(bitmap, block_number);

    if (vfs_disk_write_block(
            disk,
            VFS_BLOCK_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    put_u32_le(
        superblock + SB_FREE_BLOCKS_OFFSET,
        free_blocks + 1U
    );

    if (vfs_disk_write_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    return 0;
}

/*
 * Allocate the first available inode, starting at inode 1.
 *
 * Inode 0 is permanently reserved for the root directory.
 */
int vfs_alloc_inode(vfs_disk_t *disk, uint32_t *inode_number)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];
    uint8_t superblock[VFS_BLOCK_SIZE];

    uint32_t free_inodes;

    if (validate_disk_handle(disk) == -1) {
        return -1;
    }

    if (inode_number == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_formatted_disk(disk) == -1) {
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    free_inodes = get_u32_le(
        superblock + SB_FREE_INODES_OFFSET
    );

    if (free_inodes == 0U) {
        errno = ENOSPC;
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_INODE_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    for (uint32_t inode = 1U;
         inode < VFS_INODE_COUNT;
         inode++) {

        if (!bitmap_test(bitmap, inode)) {
            bitmap_set(bitmap, inode);

            if (vfs_disk_write_block(
                    disk,
                    VFS_INODE_BITMAP_BLOCK,
                    bitmap) == -1) {
                return -1;
            }

            put_u32_le(
                superblock + SB_FREE_INODES_OFFSET,
                free_inodes - 1U
            );

            if (vfs_disk_write_block(
                    disk,
                    VFS_SUPERBLOCK_BLOCK,
                    superblock) == -1) {
                return -1;
            }

            *inode_number = inode;

            return 0;
        }
    }

    errno = EUCLEAN;
    return -1;
}

/*
 * Free an allocated non-root inode.
 *
 * Inode data and metadata must be cleaned up by the higher-level
 * inode/file manager before this operation is called.
 */
int vfs_free_inode(vfs_disk_t *disk, uint32_t inode_number)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];
    uint8_t superblock[VFS_BLOCK_SIZE];

    uint32_t free_inodes;

    if (validate_disk_handle(disk) == -1) {
        return -1;
    }

    if (inode_number == VFS_ROOT_INODE ||
        inode_number >= VFS_INODE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    if (validate_formatted_disk(disk) == -1) {
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_INODE_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    if (!bitmap_test(bitmap, inode_number)) {
        errno = EALREADY;
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    free_inodes = get_u32_le(
        superblock + SB_FREE_INODES_OFFSET
    );

    if (free_inodes >= VFS_INODE_COUNT - 1U) {
        errno = EUCLEAN;
        return -1;
    }

    bitmap_clear(bitmap, inode_number);

    if (vfs_disk_write_block(
            disk,
            VFS_INODE_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    put_u32_le(
        superblock + SB_FREE_INODES_OFFSET,
        free_inodes + 1U
    );

    if (vfs_disk_write_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        return -1;
    }

    return 0;
}