#define _POSIX_C_SOURCE 200809L

#include "vfs_format.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * On-disk superblock field offsets.
 *
 * All multibyte integers are serialized in little-endian order.
 * These offsets are part of the filesystem format.
 */
enum {
    SB_MAGIC_OFFSET          = 0,
    SB_VERSION_OFFSET        = 8,
    SB_BLOCK_SIZE_OFFSET     = 12,
    SB_TOTAL_BLOCKS_OFFSET   = 16,
    SB_INODE_COUNT_OFFSET    = 20,
    SB_INODE_SIZE_OFFSET     = 24,
    SB_ROOT_INODE_OFFSET     = 28,
    SB_BLOCK_BITMAP_OFFSET   = 32,
    SB_INODE_BITMAP_OFFSET   = 36,
    SB_INODE_TABLE_OFFSET    = 40,
    SB_INODE_TABLE_BLOCKS    = 44,
    SB_JOURNAL_START_OFFSET  = 48,
    SB_JOURNAL_BLOCKS_OFFSET = 52,
    SB_DATA_START_OFFSET     = 56,
    SB_DATA_BLOCKS_OFFSET    = 60,
    SB_FREE_BLOCKS_OFFSET    = 64,
    SB_FREE_INODES_OFFSET    = 68,
    SB_STATE_OFFSET          = 72,
    SB_TIMESTAMP_OFFSET      = 80
};

/*
 * On-disk inode field offsets.
 *
 * Each inode occupies exactly 128 bytes.
 */
enum {
    INODE_MODE_OFFSET       = 0,
    INODE_UID_OFFSET        = 2,
    INODE_GID_OFFSET        = 4,
    INODE_LINKS_OFFSET      = 6,
    INODE_SIZE_OFFSET       = 8,
    INODE_ATIME_OFFSET      = 16,
    INODE_MTIME_OFFSET      = 24,
    INODE_CTIME_OFFSET      = 32,
    INODE_FLAGS_OFFSET      = 40,
    INODE_DIRECT_OFFSET     = 44,
    INODE_INDIRECT_OFFSET   = 76,
    INODE_RESERVED_OFFSET   = 80
};

/*
 * The root directory occupies one complete block.
 *
 * Directory entry layout:
 *   inode number : 4 bytes
 *   record length: 2 bytes
 *   file type    : 1 byte
 *   name length  : 1 byte
 *   name         : variable length
 */
enum {
    DIRENT_INODE_OFFSET     = 0,
    DIRENT_RECORD_LEN_OFFSET = 4,
    DIRENT_TYPE_OFFSET      = 6,
    DIRENT_NAME_LEN_OFFSET  = 7,
    DIRENT_NAME_OFFSET      = 8
};

/* A recognized filesystem is marked clean after formatting. */
#define VFS_STATE_CLEAN 1U

/*
 * Serialize an unsigned 16-bit integer in little-endian order.
 */
static void put_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/*
 * Serialize an unsigned 32-bit integer in little-endian order.
 */
static void put_u32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Serialize an unsigned 64-bit integer in little-endian order.
 */
static void put_u64(uint8_t *buffer, uint64_t value)
{
    for (unsigned int i = 0; i < 8U; i++) {
        buffer[i] = (uint8_t)((value >> (i * 8U)) & 0xFFU);
    }
}

/*
 * Deserialize an unsigned 16-bit little-endian integer.
 */
static uint16_t get_u16(const uint8_t *buffer)
{
    return (uint16_t)(
        (uint16_t)buffer[0] |
        ((uint16_t)buffer[1] << 8)
    );
}

/*
 * Deserialize an unsigned 32-bit little-endian integer.
 */
static uint32_t get_u32(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16) |
        ((uint32_t)buffer[3] << 24);
}

/*
 * Deserialize an unsigned 64-bit little-endian integer.
 */
static uint64_t get_u64(const uint8_t *buffer)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < 8U; i++) {
        value |= (uint64_t)buffer[i] << (i * 8U);
    }

    return value;
}

/*
 * Mark a block as allocated in the block bitmap.
 *
 * Block N is represented by bit N:
 *   byte index = N / 8
 *   bit index  = N % 8
 */
static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

/*
 * Initialize the superblock byte array.
 */
static void initialize_superblock(uint8_t *block)
{
    time_t current_time = time(NULL);
    uint64_t timestamp = current_time < (time_t)0
        ? 0U
        : (uint64_t)current_time;

    memset(block, 0, VFS_BLOCK_SIZE);

    memcpy(block + SB_MAGIC_OFFSET, VFS_MAGIC, VFS_MAGIC_SIZE);

    put_u32(block + SB_VERSION_OFFSET, VFS_FORMAT_VERSION);
    put_u32(block + SB_BLOCK_SIZE_OFFSET, VFS_BLOCK_SIZE);
    put_u32(block + SB_TOTAL_BLOCKS_OFFSET, VFS_TOTAL_BLOCKS);
    put_u32(block + SB_INODE_COUNT_OFFSET, VFS_INODE_COUNT);
    put_u32(block + SB_INODE_SIZE_OFFSET, VFS_INODE_SIZE);
    put_u32(block + SB_ROOT_INODE_OFFSET, VFS_ROOT_INODE);

    put_u32(block + SB_BLOCK_BITMAP_OFFSET, VFS_BLOCK_BITMAP_BLOCK);
    put_u32(block + SB_INODE_BITMAP_OFFSET, VFS_INODE_BITMAP_BLOCK);
    put_u32(block + SB_INODE_TABLE_OFFSET, VFS_INODE_TABLE_START);
    put_u32(block + SB_INODE_TABLE_BLOCKS, VFS_INODE_TABLE_BLOCKS);

    put_u32(block + SB_JOURNAL_START_OFFSET, VFS_JOURNAL_START);
    put_u32(block + SB_JOURNAL_BLOCKS_OFFSET, VFS_JOURNAL_BLOCKS);

    put_u32(block + SB_DATA_START_OFFSET, VFS_DATA_START);
    put_u32(block + SB_DATA_BLOCKS_OFFSET, VFS_DATA_BLOCKS);

    /*
     * Blocks 0 through 99 are allocated:
     * metadata blocks 0-98 and root directory block 99.
     */
    put_u32(
        block + SB_FREE_BLOCKS_OFFSET,
        VFS_TOTAL_BLOCKS - VFS_DATA_START - 1U
    );

    /*
     * Inode 0 is allocated to the root directory.
     */
    put_u32(
        block + SB_FREE_INODES_OFFSET,
        VFS_INODE_COUNT - 1U
    );

    put_u32(block + SB_STATE_OFFSET, VFS_STATE_CLEAN);
    put_u64(block + SB_TIMESTAMP_OFFSET, timestamp);
}

/*
 * Initialize the root inode.
 */
static void initialize_root_inode(uint8_t *inode)
{
    time_t current_time = time(NULL);
    uint64_t timestamp = current_time < (time_t)0
        ? 0U
        : (uint64_t)current_time;

    memset(inode, 0, VFS_INODE_SIZE);

    put_u16(
        inode + INODE_MODE_OFFSET,
        (uint16_t)(VFS_MODE_DIRECTORY | VFS_MODE_DEFAULT)
    );

    put_u16(inode + INODE_UID_OFFSET, 0U);
    put_u16(inode + INODE_GID_OFFSET, 0U);
    put_u16(inode + INODE_LINKS_OFFSET, 2U);

    /*
     * The root directory occupies block VFS_DATA_START.
     */
    put_u64(inode + INODE_SIZE_OFFSET, VFS_BLOCK_SIZE);
    put_u64(inode + INODE_ATIME_OFFSET, timestamp);
    put_u64(inode + INODE_MTIME_OFFSET, timestamp);
    put_u64(inode + INODE_CTIME_OFFSET, timestamp);

    put_u32(inode + INODE_FLAGS_OFFSET, 0U);

    /*
     * First direct pointer refers to the root directory block.
     * The remaining direct pointers and the indirect pointer are zero.
     */
    put_u32(
        inode + INODE_DIRECT_OFFSET,
        VFS_DATA_START
    );

    put_u32(inode + INODE_INDIRECT_OFFSET, 0U);

    /*
     * The reserved region is already zeroed by memset().
     */
    (void)INODE_RESERVED_OFFSET;
}

/*
 * Initialize the root directory block with "." and "..".
 */
static void initialize_root_directory(uint8_t *block)
{
    const uint16_t dot_record_length = 12U;
    const uint16_t dotdot_record_length =
        (uint16_t)(VFS_BLOCK_SIZE - dot_record_length);

    const uint32_t dot_offset = 0U;
    const uint32_t dotdot_offset = dot_record_length;

    memset(block, 0, VFS_BLOCK_SIZE);

    /* "." entry: the root directory refers to itself. */
    put_u32(
        block + dot_offset + DIRENT_INODE_OFFSET,
        VFS_ROOT_INODE
    );

    put_u16(
        block + dot_offset + DIRENT_RECORD_LEN_OFFSET,
        dot_record_length
    );

    block[dot_offset + DIRENT_TYPE_OFFSET] =
        VFS_DIRENT_DIRECTORY;

    block[dot_offset + DIRENT_NAME_LEN_OFFSET] = 1U;
    block[dot_offset + DIRENT_NAME_OFFSET] = '.';

    /* ".." entry: the root directory's parent is itself. */
    put_u32(
        block + dotdot_offset + DIRENT_INODE_OFFSET,
        VFS_ROOT_INODE
    );

    put_u16(
        block + dotdot_offset + DIRENT_RECORD_LEN_OFFSET,
        dotdot_record_length
    );

    block[dotdot_offset + DIRENT_TYPE_OFFSET] =
        VFS_DIRENT_DIRECTORY;

    block[dotdot_offset + DIRENT_NAME_LEN_OFFSET] = 2U;

    block[dotdot_offset + DIRENT_NAME_OFFSET] = '.';
    block[dotdot_offset + DIRENT_NAME_OFFSET + 1U] = '.';
}

/*
 * Format an open disk image.
 *
 * The superblock is written last. Before that, the image is zeroed
 * and the bitmaps, inode table, journal, and root directory are
 * initialized and synchronized.
 */
int vfs_format(vfs_disk_t *disk)
{
    uint8_t block[VFS_BLOCK_SIZE];
    uint8_t block_bitmap[VFS_BLOCK_SIZE];
    uint8_t inode_bitmap[VFS_BLOCK_SIZE];

    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    /*
     * Phase 1: zero the complete image.
     *
     * This removes stale metadata and stale file contents from a
     * previous use of the image.
     */
    memset(block, 0, sizeof(block));

    for (uint32_t i = 0; i < VFS_TOTAL_BLOCKS; i++) {
        if (vfs_disk_write_block(disk, i, block) == -1) {
            return -1;
        }
    }

    /*
     * Phase 2: initialize the block bitmap.
     *
     * Reserve blocks 0 through 98 for metadata and block 99 for
     * the root directory.
     */
    memset(block_bitmap, 0, sizeof(block_bitmap));

    for (uint32_t i = 0; i <= VFS_DATA_START; i++) {
        bitmap_set(block_bitmap, i);
    }

    if (vfs_disk_write_block(
            disk,
            VFS_BLOCK_BITMAP_BLOCK,
            block_bitmap) == -1) {
        return -1;
    }

    /*
     * Phase 3: initialize the inode bitmap.
     *
     * Inode 0 is allocated to the root directory.
     */
    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    bitmap_set(inode_bitmap, VFS_ROOT_INODE);

    if (vfs_disk_write_block(
            disk,
            VFS_INODE_BITMAP_BLOCK,
            inode_bitmap) == -1) {
        return -1;
    }

    /*
     * Phase 4: initialize the root inode.
     *
     * The inode table has already been zeroed. Read its first block,
     * populate the first 128 bytes, and write it back.
     */
    if (vfs_disk_read_block(
            disk,
            VFS_INODE_TABLE_START,
            block) == -1) {
        return -1;
    }

    initialize_root_inode(block);

    if (vfs_disk_write_block(
            disk,
            VFS_INODE_TABLE_START,
            block) == -1) {
        return -1;
    }

    /*
     * Phase 5: initialize the root directory data block.
     */
    initialize_root_directory(block);

    if (vfs_disk_write_block(
            disk,
            VFS_DATA_START,
            block) == -1) {
        return -1;
    }

    /*
     * Phase 6: persist the initialized filesystem structures before
     * publishing a valid superblock.
     */
    if (vfs_disk_sync(disk) == -1) {
        return -1;
    }

    /*
     * Phase 7: publish the superblock last.
     */
    initialize_superblock(block);

    if (vfs_disk_write_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            block) == -1) {
        return -1;
    }

    return vfs_disk_sync(disk);
}

/*
 * Validate the superblock's magic, version, and expected geometry.
 *
 * This is an initial format check, not a complete filesystem
 * consistency checker. The later fsck implementation will validate
 * allocation bitmaps, inode references, directory entries, and
 * block ownership.
 */
int vfs_is_formatted(vfs_disk_t *disk)
{
    uint8_t block[VFS_BLOCK_SIZE];

    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            block) == -1) {
        return -1;
    }

    if (memcmp(
            block + SB_MAGIC_OFFSET,
            VFS_MAGIC,
            VFS_MAGIC_SIZE) != 0) {
        return 0;
    }

    if (get_u16(block + SB_VERSION_OFFSET) != VFS_FORMAT_VERSION ||
        get_u16(block + SB_VERSION_OFFSET + 2U) != 0U ||
        get_u32(block + SB_BLOCK_SIZE_OFFSET) != VFS_BLOCK_SIZE ||
        get_u32(block + SB_TOTAL_BLOCKS_OFFSET) != VFS_TOTAL_BLOCKS ||
        get_u32(block + SB_INODE_COUNT_OFFSET) != VFS_INODE_COUNT ||
        get_u32(block + SB_INODE_SIZE_OFFSET) != VFS_INODE_SIZE ||
        get_u32(block + SB_ROOT_INODE_OFFSET) != VFS_ROOT_INODE ||
        get_u32(block + SB_BLOCK_BITMAP_OFFSET) != VFS_BLOCK_BITMAP_BLOCK ||
        get_u32(block + SB_INODE_BITMAP_OFFSET) != VFS_INODE_BITMAP_BLOCK ||
        get_u32(block + SB_INODE_TABLE_OFFSET) != VFS_INODE_TABLE_START ||
        get_u32(block + SB_INODE_TABLE_BLOCKS) != VFS_INODE_TABLE_BLOCKS ||
        get_u32(block + SB_JOURNAL_START_OFFSET) != VFS_JOURNAL_START ||
        get_u32(block + SB_JOURNAL_BLOCKS_OFFSET) != VFS_JOURNAL_BLOCKS ||
        get_u32(block + SB_DATA_START_OFFSET) != VFS_DATA_START ||
        get_u32(block + SB_DATA_BLOCKS_OFFSET) != VFS_DATA_BLOCKS ||
        get_u32(block + SB_STATE_OFFSET) != VFS_STATE_CLEAN) {
        return 0;
    }

    /*
     * The timestamp is part of the serialized format, but it is not
     * needed to determine whether the geometry is recognized.
     */
    (void)get_u64(block + SB_TIMESTAMP_OFFSET);

    return 1;
}