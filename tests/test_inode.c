#define _POSIX_C_SOURCE 200809L

#include "vfs_alloc.h"
#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_inode.img"

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
 * Compare inode fields explicitly rather than comparing the
 * entire C structure, which could contain padding bytes.
 */
static int inodes_equal(
    const vfs_inode_t *a,
    const vfs_inode_t *b
)
{
    if (a->mode != b->mode ||
        a->uid != b->uid ||
        a->gid != b->gid ||
        a->links != b->links ||
        a->size != b->size ||
        a->atime != b->atime ||
        a->mtime != b->mtime ||
        a->ctime != b->ctime ||
        a->flags != b->flags ||
        a->indirect_block != b->indirect_block) {
        return 0;
    }

    for (uint32_t i = 0U; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        if (a->direct_blocks[i] != b->direct_blocks[i]) {
            return 0;
        }
    }

    return memcmp(
        a->reserved,
        b->reserved,
        VFS_INODE_RESERVED_SIZE
    ) == 0;
}

/*
 * Construct a deterministic inode for testing.
 */
static void initialize_test_inode(
    vfs_inode_t *inode,
    uint16_t uid,
    uint64_t size
)
{
    memset(inode, 0, sizeof(*inode));

    inode->mode = (uint16_t)(VFS_MODE_REGULAR | 0644U);
    inode->uid = uid;
    inode->gid = 100U;
    inode->links = 1U;

    inode->size = size;

    inode->atime = 1700000000ULL;
    inode->mtime = 1700000100ULL;
    inode->ctime = 1700000200ULL;

    inode->flags = 7U;

    for (uint32_t i = 0U; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        inode->direct_blocks[i] = 100U + i;
    }

    inode->indirect_block = 200U;

    for (uint32_t i = 0U; i < VFS_INODE_RESERVED_SIZE; i++) {
        inode->reserved[i] = (uint8_t)i;
    }
}

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    uint32_t inode_number = UINT32_MAX;
    uint32_t second_inode_number = UINT32_MAX;

    vfs_inode_t root_inode;
    vfs_inode_t inode;
    vfs_inode_t expected_inode;
    vfs_inode_t second_inode;
    vfs_inode_t read_second_inode;

    int result;

    (void)unlink(TEST_IMAGE);

    printf("=== Inode Management Tests ===\n\n");

    /*
     * Test 1: Create the disk image.
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
     * Test 3: Format the filesystem.
     */
    result = vfs_format(&disk);

    CHECK(result == 0, "Format the filesystem");

    if (result != 0) {
        perror("Cannot format test image");
        (void)vfs_disk_close(&disk);
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    /*
     * Test 4: Read the root inode initialized by the formatter.
     */
    result = vfs_inode_read(
        &disk,
        VFS_ROOT_INODE,
        &root_inode
    );

    CHECK(result == 0, "Read the root inode");

    /*
     * Test 5: Verify that the root inode is a directory.
     */
    CHECK(
        result == 0 &&
        (root_inode.mode & 0170000U) == VFS_MODE_DIRECTORY,
        "Verify the root inode type"
    );

    /*
     * Test 6: Verify the root inode's first data block.
     */
    CHECK(
        result == 0 &&
        root_inode.direct_blocks[0] == VFS_DATA_START,
        "Verify the root inode data pointer"
    );

    /*
     * Test 7: Reject an out-of-range inode number.
     */
    errno = 0;

    result = vfs_inode_read(
        &disk,
        VFS_INODE_COUNT,
        &inode
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject an out-of-range inode number"
    );

    /*
     * Test 8: Reject an unallocated inode.
     *
     * The formatter allocates the root inode, but inode 10
     * should remain unallocated.
     */
    errno = 0;

    result = vfs_inode_read(
        &disk,
        10U,
        &inode
    );

    CHECK(
        result == -1 && errno == ENOENT,
        "Reject reading an unallocated inode"
    );

    /*
     * Test 9: Reject a NULL output pointer.
     */
    errno = 0;

    result = vfs_inode_read(
        &disk,
        VFS_ROOT_INODE,
        NULL
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a NULL inode output pointer"
    );

    /*
     * Test 10: Allocate an inode through the allocation subsystem.
     */
    result = vfs_alloc_inode(&disk, &inode_number);

    CHECK(
        result == 0 && inode_number == 1U,
        "Allocate the first non-root inode"
    );

    /*
     * Test 11: Allocate a second inode to test adjacent entries.
     */
    result = vfs_alloc_inode(&disk, &second_inode_number);

    CHECK(
        result == 0 && second_inode_number == 2U,
        "Allocate a second inode"
    );

    /*
     * Test 12: Prepare and write the second inode first.
     */
    initialize_test_inode(&second_inode, 202U, 2048U);

    result = vfs_inode_write(
        &disk,
        second_inode_number,
        &second_inode
    );

    CHECK(
        result == 0,
        "Write the second inode"
    );

    /*
     * Test 13: Prepare and write the first inode.
     */
    initialize_test_inode(&expected_inode, 101U, 12345U);

    result = vfs_inode_write(
        &disk,
        inode_number,
        &expected_inode
    );

    CHECK(
        result == 0,
        "Write an allocated inode"
    );

    /*
     * Test 14: Read the first inode and verify every field.
     */
    memset(&inode, 0, sizeof(inode));

    result = vfs_inode_read(
        &disk,
        inode_number,
        &inode
    );

    CHECK(
        result == 0 && inodes_equal(&inode, &expected_inode),
        "Read back and verify all inode fields"
    );

    /*
     * Test 15: Verify that writing inode 1 did not corrupt inode 2.
     */
    memset(&read_second_inode, 0, sizeof(read_second_inode));

    result = vfs_inode_read(
        &disk,
        second_inode_number,
        &read_second_inode
    );

    CHECK(
        result == 0 &&
        inodes_equal(&read_second_inode, &second_inode),
        "Preserve neighboring inodes in the same block"
    );

    /*
     * Test 16: Reject writing an unallocated inode.
     */
    errno = 0;

    result = vfs_inode_write(
        &disk,
        10U,
        &expected_inode
    );

    CHECK(
        result == -1 && errno == ENOENT,
        "Reject writing an unallocated inode"
    );

    /*
     * Test 17: Reject writing an out-of-range inode.
     */
    errno = 0;

    result = vfs_inode_write(
        &disk,
        VFS_INODE_COUNT,
        &expected_inode
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject writing an out-of-range inode"
    );

    /*
     * Test 18: Reject a NULL inode input.
     */
    errno = 0;

    result = vfs_inode_write(
        &disk,
        inode_number,
        NULL
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a NULL inode input"
    );

    /*
     * Test 19: Synchronize the disk image.
     */
    result = vfs_disk_sync(&disk);

    CHECK(
        result == 0,
        "Synchronize inode changes"
    );

    /*
     * Test 20: Close the disk image.
     */
    result = vfs_disk_close(&disk);

    CHECK(
        result == 0,
        "Close the disk image"
    );

    /*
     * Test 21: Reopen the disk image.
     */
    result = vfs_disk_open(&disk, TEST_IMAGE);

    CHECK(
        result == 0,
        "Reopen the disk image"
    );

    /*
     * Test 22: Verify that inode data persists after reopening.
     */
    if (result == 0) {
        memset(&inode, 0, sizeof(inode));

        result = vfs_inode_read(
            &disk,
            inode_number,
            &inode
        );

        CHECK(
            result == 0 &&
            inodes_equal(&inode, &expected_inode),
            "Preserve inode data after reopening the image"
        );
    } else {
        CHECK(
            0,
            "Preserve inode data after reopening the image"
        );
    }

    /*
     * Test 23: Close the reopened image.
     */
    result = vfs_disk_close(&disk);

    CHECK(
        result == 0,
        "Close the reopened disk image"
    );

    (void)unlink(TEST_IMAGE);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}