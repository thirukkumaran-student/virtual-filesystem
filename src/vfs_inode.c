#define _POSIX_C_SOURCE 200809L

#include "vfs_inode.h"
#include "vfs_format.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * On-disk inode field offsets.
 *
 * Total inode size: 128 bytes.
 */
#define INODE_MODE_OFFSET       0U
#define INODE_UID_OFFSET        2U
#define INODE_GID_OFFSET        4U
#define INODE_LINKS_OFFSET      6U
#define INODE_SIZE_OFFSET       8U
#define INODE_ATIME_OFFSET      16U
#define INODE_MTIME_OFFSET      24U
#define INODE_CTIME_OFFSET      32U
#define INODE_FLAGS_OFFSET      40U
#define INODE_DIRECT_OFFSET     44U
#define INODE_INDIRECT_OFFSET   76U
#define INODE_RESERVED_OFFSET   80U

#define INODE_BITMAP_BLOCK VFS_INODE_BITMAP_BLOCK

/*
 * Encode a 16-bit integer in little-endian format.
 */
static void put_u16_le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/*
 * Decode a 16-bit little-endian integer.
 */
static uint16_t get_u16_le(const uint8_t *buffer)
{
    return (uint16_t)(
        (uint16_t)buffer[0] |
        ((uint16_t)buffer[1] << 8)
    );
}

/*
 * Encode a 32-bit integer in little-endian format.
 */
static void put_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Decode a 32-bit little-endian integer.
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
 * Encode a 64-bit integer in little-endian format.
 */
static void put_u64_le(uint8_t *buffer, uint64_t value)
{
    for (uint32_t i = 0U; i < 8U; i++) {
        buffer[i] = (uint8_t)(value & 0xFFU);
        value >>= 8;
    }
}

/*
 * Decode a 64-bit little-endian integer.
 */
static uint64_t get_u64_le(const uint8_t *buffer)
{
    uint64_t value = 0U;

    for (uint32_t i = 0U; i < 8U; i++) {
        value |= (uint64_t)buffer[i] << (i * 8U);
    }

    return value;
}

/*
 * Validate the disk handle and filesystem format.
 */
static int validate_disk(vfs_disk_t *disk)
{
    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (vfs_is_formatted(disk) != 1) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/*
 * Determine whether an inode is marked allocated in the inode bitmap.
 */
static int inode_is_allocated(
    vfs_disk_t *disk,
    uint32_t inode_number
)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk,
            INODE_BITMAP_BLOCK,
            bitmap) == -1) {
        return -1;
    }

    uint32_t byte_index = inode_number / 8U;
    uint32_t bit_index = inode_number % 8U;

    return
        (bitmap[byte_index] &
         (uint8_t)(1U << bit_index)) != 0U;
}

/*
 * Validate an inode number and confirm that the inode is allocated.
 */
static int validate_inode_number(
    vfs_disk_t *disk,
    uint32_t inode_number
)
{
    if (inode_number >= VFS_INODE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    int allocated = inode_is_allocated(disk, inode_number);

    if (allocated == -1) {
        return -1;
    }

    if (allocated == 0) {
        errno = ENOENT;
        return -1;
    }

    return 0;
}

/*
 * Serialize an in-memory inode into the exact 128-byte disk format.
 */
static void serialize_inode(
    const vfs_inode_t *inode,
    uint8_t buffer[VFS_INODE_SIZE]
)
{
    memset(buffer, 0, VFS_INODE_SIZE);

    put_u16_le(buffer + INODE_MODE_OFFSET, inode->mode);
    put_u16_le(buffer + INODE_UID_OFFSET, inode->uid);
    put_u16_le(buffer + INODE_GID_OFFSET, inode->gid);
    put_u16_le(buffer + INODE_LINKS_OFFSET, inode->links);

    put_u64_le(buffer + INODE_SIZE_OFFSET, inode->size);
    put_u64_le(buffer + INODE_ATIME_OFFSET, inode->atime);
    put_u64_le(buffer + INODE_MTIME_OFFSET, inode->mtime);
    put_u64_le(buffer + INODE_CTIME_OFFSET, inode->ctime);

    put_u32_le(buffer + INODE_FLAGS_OFFSET, inode->flags);

    for (uint32_t i = 0U; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        put_u32_le(
            buffer + INODE_DIRECT_OFFSET + (i * 4U),
            inode->direct_blocks[i]
        );
    }

    put_u32_le(
        buffer + INODE_INDIRECT_OFFSET,
        inode->indirect_block
    );

    memcpy(
        buffer + INODE_RESERVED_OFFSET,
        inode->reserved,
        VFS_INODE_RESERVED_SIZE
    );
}

/*
 * Deserialize the exact 128-byte disk representation into an inode.
 */
static void deserialize_inode(
    const uint8_t buffer[VFS_INODE_SIZE],
    vfs_inode_t *inode
)
{
    memset(inode, 0, sizeof(*inode));

    inode->mode = get_u16_le(buffer + INODE_MODE_OFFSET);
    inode->uid = get_u16_le(buffer + INODE_UID_OFFSET);
    inode->gid = get_u16_le(buffer + INODE_GID_OFFSET);
    inode->links = get_u16_le(buffer + INODE_LINKS_OFFSET);

    inode->size = get_u64_le(buffer + INODE_SIZE_OFFSET);
    inode->atime = get_u64_le(buffer + INODE_ATIME_OFFSET);
    inode->mtime = get_u64_le(buffer + INODE_MTIME_OFFSET);
    inode->ctime = get_u64_le(buffer + INODE_CTIME_OFFSET);

    inode->flags = get_u32_le(buffer + INODE_FLAGS_OFFSET);

    for (uint32_t i = 0U; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        inode->direct_blocks[i] = get_u32_le(
            buffer + INODE_DIRECT_OFFSET + (i * 4U)
        );
    }

    inode->indirect_block = get_u32_le(
        buffer + INODE_INDIRECT_OFFSET
    );

    memcpy(
        inode->reserved,
        buffer + INODE_RESERVED_OFFSET,
        VFS_INODE_RESERVED_SIZE
    );
}

/*
 * Calculate the inode table block and offset for an inode number.
 *
 * There are 32 inodes per 4096-byte block.
 */
static void locate_inode(
    uint32_t inode_number,
    uint32_t *block_number,
    uint32_t *byte_offset
)
{
    *block_number =
        VFS_INODE_TABLE_START +
        (inode_number / (VFS_BLOCK_SIZE / VFS_INODE_SIZE));

    *byte_offset =
        (inode_number % (VFS_BLOCK_SIZE / VFS_INODE_SIZE)) *
        VFS_INODE_SIZE;
}

/*
 * Read an allocated inode from disk.
 */
int vfs_inode_read(
    vfs_disk_t *disk,
    uint32_t inode_number,
    vfs_inode_t *inode
)
{
    uint8_t block[VFS_BLOCK_SIZE];
    uint8_t serialized[VFS_INODE_SIZE];

    uint32_t block_number;
    uint32_t byte_offset;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (validate_inode_number(disk, inode_number) == -1) {
        return -1;
    }

    locate_inode(
        inode_number,
        &block_number,
        &byte_offset
    );

    if (vfs_disk_read_block(
            disk,
            block_number,
            block) == -1) {
        return -1;
    }

    memcpy(
        serialized,
        block + byte_offset,
        VFS_INODE_SIZE
    );

    deserialize_inode(serialized, inode);

    return 0;
}

/*
 * Write an allocated inode to disk.
 *
 * The entire inode-table block is read and written back so that
 * neighboring inodes in the same block remain unchanged.
 */
int vfs_inode_write(
    vfs_disk_t *disk,
    uint32_t inode_number,
    const vfs_inode_t *inode
)
{
    uint8_t block[VFS_BLOCK_SIZE];
    uint8_t serialized[VFS_INODE_SIZE];

    uint32_t block_number;
    uint32_t byte_offset;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (validate_inode_number(disk, inode_number) == -1) {
        return -1;
    }

    locate_inode(
        inode_number,
        &block_number,
        &byte_offset
    );

    if (vfs_disk_read_block(
            disk,
            block_number,
            block) == -1) {
        return -1;
    }

    serialize_inode(inode, serialized);

    memcpy(
        block + byte_offset,
        serialized,
        VFS_INODE_SIZE
    );

    if (vfs_disk_write_block(
            disk,
            block_number,
            block) == -1) {
        return -1;
    }

    return 0;
}