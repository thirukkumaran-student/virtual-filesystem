#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_disk.img"
#define INVALID_IMAGE "build/invalid_disk.img"

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition, description)                              \
    do {                                                           \
        if (condition) {                                           \
            printf("[PASS] %s\n", description);                    \
            tests_passed++;                                        \
        } else {                                                   \
            printf("[FAIL] %s (line %d)\n", description, __LINE__);\
            tests_failed++;                                        \
        }                                                          \
    } while (0)

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    unsigned char write_buffer[VFS_BLOCK_SIZE];
    unsigned char read_buffer[VFS_BLOCK_SIZE];

    struct stat file_info;
    FILE *invalid_file;

    int result;

    /*
     * Remove artifacts from an earlier test run.
     */
    (void)unlink(TEST_IMAGE);
    (void)unlink(INVALID_IMAGE);

    printf("=== Virtual Disk Unit Tests ===\n\n");

    /*
     * Test 1: Create a disk image.
     */
    result = vfs_disk_create(TEST_IMAGE);

    CHECK(result == 0, "Create a new disk image");

    /*
     * Test 2: Verify the image size.
     */
    result = stat(TEST_IMAGE, &file_info);

    CHECK(
        result == 0 &&
        file_info.st_size == (off_t)VFS_DISK_SIZE,
        "Disk image has the expected size"
    );

    /*
     * Test 3: Existing images must not be overwritten.
     */
    errno = 0;
    result = vfs_disk_create(TEST_IMAGE);

    CHECK(
        result == -1 && errno == EEXIST,
        "Reject creation when the image already exists"
    );

    /*
     * Test 4: Open the existing image.
     */
    result = vfs_disk_open(&disk, TEST_IMAGE);

    CHECK(
        result == 0 && disk.is_open,
        "Open an existing disk image"
    );

    /*
     * Test 5: Write a known pattern to a data block.
     */
    for (size_t i = 0; i < VFS_BLOCK_SIZE; i++) {
        write_buffer[i] = (unsigned char)(i % 251U);
    }

    result = vfs_disk_write_block(&disk, 100, write_buffer);

    CHECK(result == 0, "Write one complete block");

    /*
     * Test 6: Synchronize pending changes.
     */
    result = vfs_disk_sync(&disk);

    CHECK(result == 0, "Synchronize disk-image changes");

    /*
     * Test 7: Read the block and compare every byte.
     */
    memset(read_buffer, 0, sizeof(read_buffer));

    result = vfs_disk_read_block(&disk, 100, read_buffer);

    CHECK(
        result == 0 &&
        memcmp(write_buffer, read_buffer, VFS_BLOCK_SIZE) == 0,
        "Read back the block and verify its contents"
    );

    /*
     * Test 8: Reject an out-of-range block number.
     */
    errno = 0;

    result = vfs_disk_read_block(
        &disk,
        VFS_TOTAL_BLOCKS,
        read_buffer
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject an out-of-range block number"
    );

    /*
     * Test 9: Reject a NULL buffer.
     */
    errno = 0;

    result = vfs_disk_read_block(&disk, 0, NULL);

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a NULL read buffer"
    );

    /*
     * Test 10: Close the disk.
     */
    result = vfs_disk_close(&disk);

    CHECK(
        result == 0 && !disk.is_open && disk.fd == -1,
        "Close and invalidate the disk handle"
    );

    /*
     * Test 11: Reject operations on a closed handle.
     */
    errno = 0;

    result = vfs_disk_read_block(&disk, 0, read_buffer);

    CHECK(
        result == -1 && errno == EBADF,
        "Reject reads through a closed disk handle"
    );

    /*
     * Test 12: Reject an image with the wrong size.
     */
    invalid_file = fopen(INVALID_IMAGE, "wb");

    if (invalid_file != NULL) {
        (void)fclose(invalid_file);
    }

    errno = 0;

    result = vfs_disk_open(&disk, INVALID_IMAGE);

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a disk image with an incorrect size"
    );

    /*
     * Remove generated test images.
     */
    (void)unlink(TEST_IMAGE);
    (void)unlink(INVALID_IMAGE);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}