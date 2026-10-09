#define _POSIX_C_SOURCE 200809L

#include "vfs_alloc.h"
#include "vfs_format.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_alloc.img"

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

/*
 * Read a 32-bit unsigned integer in little-endian format.
 */
static uint32_t read_u32_le(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16) |
        ((uint32_t)buffer[3] << 24);
}

/*
 * Check whether a particular bitmap bit is set.
 */
static int bitmap_is_set(const uint8_t *bitmap, uint32_t bit)
{
    return
        (bitmap[bit / 8U] &
         (uint8_t)(1U << (bit % 8U))) != 0U;
}

/*
 * Read a 32-bit counter from the superblock.
 *
 * Offset 64: free-block count.
 * Offset 68: free-inode count.
 */
static uint32_t read_superblock_counter(
    vfs_disk_t *disk,
    uint32_t offset
)
{
    uint8_t block[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            block) == -1) {
        return UINT32_MAX;
    }

    return read_u32_le(block + offset);
}

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    uint32_t block_number = UINT32_MAX;
    uint32_t inode_number = UINT32_MAX;

    uint8_t bitmap[VFS_BLOCK_SIZE];

    int result;

    /*
     * Remove any image left by a previous test run.
     */
    (void)unlink(TEST_IMAGE);

    printf("=== Allocation Tests ===\n\n");

    /*
     * Test 1: Create a disk image.
     */
    result = vfs_disk_create(TEST_IMAGE);

    CHECK(result == 0, "Create a disk image");

    if (result != 0) {
        perror("Cannot create test image");
        return EXIT_FAILURE;
    }

    /*
     * Test 2: Open the disk image.
     */
    result = vfs_disk_open(&disk, TEST_IMAGE);

    CHECK(result == 0, "Open the disk image");

    if (result != 0) {
        perror("Cannot open test image");
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    /*
     * Test 3: Format the disk image.
     */
    result = vfs_format(&disk);

    CHECK(result == 0, "Format the disk image");

    if (result != 0) {
        perror("Cannot format test image");
        (void)vfs_disk_close(&disk);
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    /*
     * Test 4: Verify the initial free-block count.
     *
     * The data region contains 3997 blocks.
     * Block 99 is reserved for the root directory.
     * Therefore, 3996 blocks are initially free.
     */
    CHECK(
        read_superblock_counter(&disk, 64U) ==
            VFS_DATA_BLOCKS - 1U,
        "Verify the initial free-block count"
    );

    /*
     * Test 5: Verify the initial free-inode count.
     */
    CHECK(
        read_superblock_counter(&disk, 68U) ==
            VFS_INODE_COUNT - 1U,
        "Verify the initial free-inode count"
    );

    /*
     * Test 6: Allocate the first available data block.
     */
    result = vfs_alloc_block(&disk, &block_number);

    CHECK(
        result == 0 &&
        block_number == VFS_DATA_START + 1U,
        "Allocate the first free data block"
    );

    /*
     * Test 7: Verify the free-block counter was decremented.
     */
    CHECK(
        read_superblock_counter(&disk, 64U) ==
            VFS_DATA_BLOCKS - 2U,
        "Decrement the free-block count"
    );

    /*
     * Test 8: Verify the block bitmap.
     */
    result = vfs_disk_read_block(
        &disk,
        VFS_BLOCK_BITMAP_BLOCK,
        bitmap
    );

    CHECK(
        result == 0 &&
        bitmap_is_set(bitmap, block_number),
        "Mark the allocated block in the bitmap"
    );

    /*
     * Test 9: Allocate another block.
     */
    {
        uint32_t second_block = UINT32_MAX;

        result = vfs_alloc_block(&disk, &second_block);

        CHECK(
            result == 0 &&
            second_block == block_number + 1U,
            "Allocate a different block on the next request"
        );
    }

    /*
     * Test 10: Free the first allocated block.
     */
    result = vfs_free_block(&disk, block_number);

    CHECK(
        result == 0,
        "Free an allocated data block"
    );

    /*
     * Test 11: Verify the free-block counter.
     *
     * Two blocks were allocated and one was freed.
     * One block remains allocated, so the expected free count is:
     *
     * VFS_DATA_BLOCKS - 2 = 3995.
     */
    CHECK(
        read_superblock_counter(&disk, 64U) ==
            VFS_DATA_BLOCKS - 2U,
        "Increment the free-block count"
    );

    /*
     * Test 12: Verify the freed block's bitmap bit is cleared.
     */
    result = vfs_disk_read_block(
        &disk,
        VFS_BLOCK_BITMAP_BLOCK,
        bitmap
    );

    CHECK(
        result == 0 &&
        !bitmap_is_set(bitmap, block_number),
        "Clear the freed block's bitmap bit"
    );

    /*
     * Test 13: Reject a double free.
     */
    errno = 0;

    result = vfs_free_block(&disk, block_number);

    CHECK(
        result == -1 &&
        errno == EALREADY,
        "Reject a double free"
    );

    /*
     * Test 14: Reject freeing the superblock.
     */
    errno = 0;

    result = vfs_free_block(
        &disk,
        VFS_SUPERBLOCK_BLOCK
    );

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject freeing the superblock"
    );

    /*
     * Test 15: Reject freeing the root directory block.
     */
    errno = 0;

    result = vfs_free_block(
        &disk,
        VFS_DATA_START
    );

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject freeing the root directory block"
    );

    /*
     * Test 16: Reject an out-of-range block.
     */
    errno = 0;

    result = vfs_free_block(
        &disk,
        VFS_TOTAL_BLOCKS
    );

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject an out-of-range block"
    );

    /*
     * Test 17: Allocate the first available non-root inode.
     */
    result = vfs_alloc_inode(&disk, &inode_number);

    CHECK(
        result == 0 &&
        inode_number == 1U,
        "Allocate the first available non-root inode"
    );

    /*
     * Test 18: Verify the free-inode counter was decremented.
     */
    CHECK(
        read_superblock_counter(&disk, 68U) ==
            VFS_INODE_COUNT - 2U,
        "Decrement the free-inode count"
    );

    /*
     * Test 19: Verify the inode bitmap.
     */
    result = vfs_disk_read_block(
        &disk,
        VFS_INODE_BITMAP_BLOCK,
        bitmap
    );

    CHECK(
        result == 0 &&
        bitmap_is_set(bitmap, inode_number),
        "Mark the allocated inode in the bitmap"
    );

    /*
     * Test 20: Free the allocated inode.
     */
    result = vfs_free_inode(&disk, inode_number);

    CHECK(
        result == 0,
        "Free an allocated inode"
    );

    /*
     * Test 21: Verify the free-inode counter was incremented.
     *
     * This must read offset 68, not offset 64.
     */
    CHECK(
        read_superblock_counter(&disk, 68U) ==
            VFS_INODE_COUNT - 1U,
        "Increment the free-inode count"
    );

    /*
     * Test 22: Reject freeing the same inode twice.
     */
    errno = 0;

    result = vfs_free_inode(&disk, inode_number);

    CHECK(
        result == -1 &&
        errno == EALREADY,
        "Reject freeing the same inode twice"
    );

    /*
     * Test 23: Reject freeing the root inode.
     */
    errno = 0;

    result = vfs_free_inode(
        &disk,
        VFS_ROOT_INODE
    );

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject freeing the root inode"
    );

    /*
     * Test 24: Reject an out-of-range inode.
     */
    errno = 0;

    result = vfs_free_inode(
        &disk,
        VFS_INODE_COUNT
    );

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject an out-of-range inode"
    );

    /*
     * Test 25: Reject a NULL block-number output pointer.
     */
    errno = 0;

    result = vfs_alloc_block(&disk, NULL);

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject a NULL block-number output"
    );

    /*
     * Test 26: Reject a NULL inode-number output pointer.
     */
    errno = 0;

    result = vfs_alloc_inode(&disk, NULL);

    CHECK(
        result == -1 &&
        errno == EINVAL,
        "Reject a NULL inode-number output"
    );

    /*
     * Test 27: Synchronize metadata.
     */
    result = vfs_disk_sync(&disk);

    CHECK(
        result == 0,
        "Synchronize allocation metadata"
    );

    /*
     * Test 28: Close the disk image.
     */
    result = vfs_disk_close(&disk);

    CHECK(
        result == 0,
        "Close the disk image"
    );

    /*
     * Remove the temporary image.
     */
    (void)unlink(TEST_IMAGE);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}