#define _POSIX_C_SOURCE 200809L

#include "vfs_format.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_format.img"

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition, description)                                \
    do {                                                             \
        if (condition) {                                             \
            printf("[PASS] %s\n", description);                      \
            tests_passed++;                                          \
        } else {                                                     \
            printf("[FAIL] %s (line %d)\n", description, __LINE__);  \
            tests_failed++;                                          \
        }                                                            \
    } while (0)

static uint16_t read_u16_le(const uint8_t *buffer)
{
    return (uint16_t)(
        (uint16_t)buffer[0] |
        ((uint16_t)buffer[1] << 8)
    );
}

static uint32_t read_u32_le(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16) |
        ((uint32_t)buffer[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *buffer)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < 8U; i++) {
        value |= (uint64_t)buffer[i] << (i * 8U);
    }

    return value;
}

static int bitmap_is_set(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    uint8_t block[VFS_BLOCK_SIZE];
    int result;

    (void)unlink(TEST_IMAGE);

    printf("=== Filesystem Formatter Tests ===\n\n");

    /* Test 1: Create the backing image. */
    result = vfs_disk_create(TEST_IMAGE);

    CHECK(result == 0, "Create a disk image");

    if (result != 0) {
        perror("Cannot create test image");
        return EXIT_FAILURE;
    }

    /* Test 2: Open the image. */
    result = vfs_disk_open(&disk, TEST_IMAGE);

    CHECK(result == 0, "Open the disk image");

    if (result != 0) {
        perror("Cannot open test image");
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    /* Test 3: A new image must not be recognized as formatted. */
    result = vfs_is_formatted(&disk);

    CHECK(result == 0, "Reject an unformatted disk image");

    /* Test 4: Format the image. */
    result = vfs_format(&disk);

    CHECK(result == 0, "Format the disk image");

    if (result != 0) {
        perror("Formatting failed");
        (void)vfs_disk_close(&disk);
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    /* Test 5: Recognize the initialized filesystem. */
    result = vfs_is_formatted(&disk);

    CHECK(result == 1, "Recognize a formatted filesystem");

    /* Test 6: Validate superblock magic. */
    result = vfs_disk_read_block(
        &disk,
        VFS_SUPERBLOCK_BLOCK,
        block
    );

    CHECK(
        result == 0 &&
        memcmp(block, VFS_MAGIC, VFS_MAGIC_SIZE) == 0,
        "Write the correct superblock magic"
    );

    /* Test 7: Validate superblock geometry. */
    CHECK(
        read_u32_le(block + 8U) == VFS_FORMAT_VERSION &&
        read_u32_le(block + 12U) == VFS_BLOCK_SIZE &&
        read_u32_le(block + 16U) == VFS_TOTAL_BLOCKS &&
        read_u32_le(block + 20U) == VFS_INODE_COUNT &&
        read_u32_le(block + 24U) == VFS_INODE_SIZE,
        "Store the expected filesystem geometry"
    );

    /* Test 8: Validate the free-space counters. */
    CHECK(
        read_u32_le(block + 64U) ==
            VFS_TOTAL_BLOCKS - VFS_DATA_START - 1U &&
        read_u32_le(block + 68U) == VFS_INODE_COUNT - 1U,
        "Initialize free-block and free-inode counters"
    );

    /* Test 9: Validate the block bitmap. */
    result = vfs_disk_read_block(
        &disk,
        VFS_BLOCK_BITMAP_BLOCK,
        block
    );

    CHECK(result == 0, "Read the block bitmap");

    if (result == 0) {
        int bitmap_valid = 1;

        /*
         * Blocks 0 through 99 must be allocated.
         */
        for (uint32_t i = 0; i <= VFS_DATA_START; i++) {
            if (!bitmap_is_set(block, i)) {
                bitmap_valid = 0;
                break;
            }
        }

        /*
         * Block 100 is the first free block.
         */
        if (bitmap_is_set(block, VFS_DATA_START + 1U)) {
            bitmap_valid = 0;
        }

        CHECK(
            bitmap_valid,
            "Reserve metadata blocks and the root directory block"
        );
    } else {
        CHECK(0, "Reserve metadata blocks and the root directory block");
    }

    /* Test 10: Validate the inode bitmap. */
    result = vfs_disk_read_block(
        &disk,
        VFS_INODE_BITMAP_BLOCK,
        block
    );

    CHECK(result == 0, "Read the inode bitmap");

    if (result == 0) {
        CHECK(
            bitmap_is_set(block, VFS_ROOT_INODE) &&
            !bitmap_is_set(block, VFS_ROOT_INODE + 1U),
            "Allocate the root inode and leave the next inode free"
        );
    } else {
        CHECK(
            0,
            "Allocate the root inode and leave the next inode free"
        );
    }

    /* Test 11: Validate the root inode. */
    result = vfs_disk_read_block(
        &disk,
        VFS_INODE_TABLE_START,
        block
    );

    CHECK(result == 0, "Read the root inode table block");

    if (result == 0) {
        const uint8_t *inode = block;

        CHECK(
            read_u16_le(inode) ==
                (uint16_t)(VFS_MODE_DIRECTORY | VFS_MODE_DEFAULT) &&
            read_u16_le(inode + 6U) == 2U &&
            read_u64_le(inode + 8U) == VFS_BLOCK_SIZE &&
            read_u32_le(inode + 44U) == VFS_DATA_START,
            "Initialize root inode metadata and data pointer"
        );
    } else {
        CHECK(0, "Initialize root inode metadata and data pointer");
    }

    /* Test 12: Validate the root directory entries. */
    result = vfs_disk_read_block(&disk, VFS_DATA_START, block);

    CHECK(result == 0, "Read the root directory block");

    if (result == 0) {
        int dot_valid =
            read_u32_le(block) == VFS_ROOT_INODE &&
            read_u16_le(block + 4U) == 12U &&
            block[6U] == VFS_DIRENT_DIRECTORY &&
            block[7U] == 1U &&
            block[8U] == '.';

        int dotdot_valid =
            read_u32_le(block + 12U) == VFS_ROOT_INODE &&
            read_u16_le(block + 16U) ==
                (uint16_t)(VFS_BLOCK_SIZE - 12U) &&
            block[18U] == VFS_DIRENT_DIRECTORY &&
            block[19U] == 2U &&
            block[20U] == '.' &&
            block[21U] == '.';

        CHECK(
            dot_valid && dotdot_valid,
            "Initialize the root directory entries"
        );
    } else {
        CHECK(0, "Initialize the root directory entries");
    }

    /* Test 13: Reject a corrupted superblock. */
    memset(block, 0, sizeof(block));

    result = vfs_disk_write_block(
        &disk,
        VFS_SUPERBLOCK_BLOCK,
        block
    );

    if (result == 0) {
        result = vfs_is_formatted(&disk);

        CHECK(
            result == 0,
            "Reject a corrupted superblock"
        );
    } else {
        CHECK(0, "Reject a corrupted superblock");
    }

    /* Close and remove the temporary test image. */
    result = vfs_disk_close(&disk);

    CHECK(result == 0, "Close the disk image");

    (void)unlink(TEST_IMAGE);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}