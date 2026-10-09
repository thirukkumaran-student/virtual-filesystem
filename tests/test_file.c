#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_dir.h"
#include "vfs_file.h"
#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_file.img"

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

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    uint32_t file_inode = UINT32_MAX;
    uint32_t found_inode = UINT32_MAX;

    uint8_t source[5000];
    uint8_t output[5000];

    int result;
    int64_t io_result;

    (void)unlink(TEST_IMAGE);

    printf("=== File Management Tests ===\n\n");

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
     * Test 4: Create a regular file in the root directory.
     */
    result = vfs_file_create(
        &disk,
        VFS_ROOT_INODE,
        "sample.txt",
        &file_inode
    );

    CHECK(
        result == 0 && file_inode != VFS_ROOT_INODE,
        "Create a regular file"
    );

    /*
     * Test 5: Look up the file through the directory API.
     */
    result = vfs_dir_lookup(
        &disk,
        VFS_ROOT_INODE,
        "sample.txt",
        &found_inode
    );

    CHECK(
        result == 0 && found_inode == file_inode,
        "Find the file through its directory entry"
    );

    /*
     * Test 6: Verify the initial file size.
     */
    {
        vfs_inode_t inode;

        result = vfs_inode_read(
            &disk,
            file_inode,
            &inode
        );

        CHECK(
            result == 0 && inode.size == 0U,
            "Initialize a new file with zero size"
        );
    }

    /*
     * Prepare a deterministic 5000-byte payload.
     */
    for (size_t i = 0U; i < sizeof(source); i++) {
        source[i] = (uint8_t)((i * 37U + 11U) % 251U);
    }

    /*
     * Test 7: Write data spanning two blocks.
     */
    io_result = vfs_file_write(
        &disk,
        file_inode,
        source,
        sizeof(source),
        0U
    );

    CHECK(
        io_result == (int64_t)sizeof(source),
        "Write file data across a block boundary"
    );

    /*
     * Test 8: Verify the updated file size.
     */
    {
        vfs_inode_t inode;

        result = vfs_inode_read(
            &disk,
            file_inode,
            &inode
        );

        CHECK(
            result == 0 && inode.size == sizeof(source),
            "Update the file size after writing"
        );
    }

    /*
     * Test 9: Read back the entire file.
     */
    memset(output, 0, sizeof(output));

    io_result = vfs_file_read(
        &disk,
        file_inode,
        output,
        sizeof(output),
        0U
    );

    CHECK(
        io_result == (int64_t)sizeof(output) &&
        memcmp(source, output, sizeof(source)) == 0,
        "Read back and verify file contents"
    );

    /*
     * Test 10: Read a range that crosses the block boundary.
     */
    {
        uint8_t slice[32];

        memset(slice, 0, sizeof(slice));

        io_result = vfs_file_read(
            &disk,
            file_inode,
            slice,
            sizeof(slice),
            4080U
        );

        CHECK(
            io_result == (int64_t)sizeof(slice) &&
            memcmp(slice, source + 4080U, sizeof(slice)) == 0,
            "Read a range spanning two blocks"
        );
    }

    /*
     * Test 11: Read beyond end-of-file.
     */
    io_result = vfs_file_read(
        &disk,
        file_inode,
        output,
        10U,
        6000U
    );

    CHECK(
        io_result == 0,
        "Return zero when reading beyond end-of-file"
    );

    /*
     * Test 12: Reject a NULL read buffer for a nonzero count.
     */
    errno = 0;

    io_result = vfs_file_read(
        &disk,
        file_inode,
        NULL,
        10U,
        0U
    );

    CHECK(
        io_result == -1 && errno == EINVAL,
        "Reject a NULL read buffer"
    );

    /*
     * Test 13: Reject a sparse write beyond end-of-file.
     */
    errno = 0;

    io_result = vfs_file_write(
        &disk,
        file_inode,
        "x",
        1U,
        6000U
    );

    CHECK(
        io_result == -1 && errno == EINVAL,
        "Reject a write that creates a sparse gap"
    );

    /*
     * Test 14: Reject a NULL write buffer for a nonzero count.
     */
    errno = 0;

    io_result = vfs_file_write(
        &disk,
        file_inode,
        NULL,
        1U,
        0U
    );

    CHECK(
        io_result == -1 && errno == EINVAL,
        "Reject a NULL write buffer"
    );

    /*
     * Test 15: Reject a write exceeding the direct-block limit.
     */
    errno = 0;

    io_result = vfs_file_write(
        &disk,
        file_inode,
        source,
        1U,
        VFS_FILE_MAX_SIZE
    );

    CHECK(
        io_result == -1 && errno == EFBIG,
        "Reject a write beyond the maximum file size"
    );

    /*
     * Test 16: Overwrite a range inside the file.
     */
    {
        const char replacement[] = "FILESYSTEM";

        io_result = vfs_file_write(
            &disk,
            file_inode,
            replacement,
            sizeof(replacement) - 1U,
            100U
        );

        CHECK(
            io_result == (int64_t)(sizeof(replacement) - 1U),
            "Overwrite existing file data"
        );

        char verify[sizeof(replacement)];

        memset(verify, 0, sizeof(verify));

        io_result = vfs_file_read(
            &disk,
            file_inode,
            verify,
            sizeof(replacement) - 1U,
            100U
        );

        CHECK(
            io_result == (int64_t)(sizeof(replacement) - 1U) &&
            memcmp(
                verify,
                replacement,
                sizeof(replacement) - 1U
            ) == 0,
            "Verify overwritten file data"
        );
    }

    /*
     * Test 17: Truncate the file to three bytes.
     */
    result = vfs_file_truncate(
        &disk,
        file_inode,
        3U
    );

    CHECK(
        result == 0,
        "Shrink a file"
    );

    /*
     * Test 18: Verify the shortened file size.
     */
    {
        vfs_inode_t inode;

        result = vfs_inode_read(
            &disk,
            file_inode,
            &inode
        );

        CHECK(
            result == 0 && inode.size == 3U,
            "Update the file size after truncation"
        );
    }

    /*
     * Test 19: Verify the first three bytes remain unchanged.
     */
    memset(output, 0, sizeof(output));

    io_result = vfs_file_read(
        &disk,
        file_inode,
        output,
        3U,
        0U
    );

    CHECK(
        io_result == 3 &&
        memcmp(output, source, 3U) == 0,
        "Preserve the retained bytes after truncation"
    );

    /*
     * Test 20: Extend the file.
     */
    result = vfs_file_truncate(
        &disk,
        file_inode,
        10U
    );

    CHECK(
        result == 0,
        "Extend a file"
    );

    /*
     * Test 21: Verify newly exposed bytes are zero-filled.
     */
    memset(output, 0xFF, 10U);

    io_result = vfs_file_read(
        &disk,
        file_inode,
        output,
        10U,
        0U
    );

    int zero_filled = 1;

    if (io_result == 10) {
        for (size_t i = 3U; i < 10U; i++) {
            if (output[i] != 0U) {
                zero_filled = 0;
                break;
            }
        }
    } else {
        zero_filled = 0;
    }

    CHECK(
        io_result == 10 &&
        memcmp(output, source, 3U) == 0 &&
        zero_filled,
        "Zero-fill bytes exposed by file extension"
    );

    /*
     * Test 22: Reject truncation beyond the direct-block limit.
     */
    errno = 0;

    result = vfs_file_truncate(
        &disk,
        file_inode,
        VFS_FILE_MAX_SIZE + 1U
    );

    CHECK(
        result == -1 && errno == EFBIG,
        "Reject an oversized truncation request"
    );

    /*
     * Test 23: Reject reading the root directory as a regular file.
     */
    errno = 0;

    io_result = vfs_file_read(
        &disk,
        VFS_ROOT_INODE,
        output,
        1U,
        0U
    );

    CHECK(
        io_result == -1 && errno == EISDIR,
        "Reject reading a directory as a regular file"
    );

    /*
     * Test 24: Synchronize changes.
     */
    result = vfs_disk_sync(&disk);

    CHECK(
        result == 0,
        "Synchronize file changes"
    );

    /*
     * Test 25: Close the disk image.
     */
    result = vfs_disk_close(&disk);

    CHECK(
        result == 0,
        "Close the disk image"
    );

    /*
     * Test 26: Reopen the disk image.
     */
    result = vfs_disk_open(&disk, TEST_IMAGE);

    CHECK(
        result == 0,
        "Reopen the disk image"
    );

    /*
     * Test 27: Verify file size and contents persist.
     */
    if (result == 0) {
        vfs_inode_t inode;

        result = vfs_inode_read(
            &disk,
            file_inode,
            &inode
        );

        CHECK(
            result == 0 && inode.size == 10U,
            "Preserve file metadata after reopening"
        );

        memset(output, 0xFF, 10U);

        io_result = vfs_file_read(
            &disk,
            file_inode,
            output,
            10U,
            0U
        );

        int persistent_zeros = 1;

        if (io_result == 10) {
            for (size_t i = 3U; i < 10U; i++) {
                if (output[i] != 0U) {
                    persistent_zeros = 0;
                    break;
                }
            }
        } else {
            persistent_zeros = 0;
        }

        CHECK(
            io_result == 10 &&
            memcmp(output, source, 3U) == 0 &&
            persistent_zeros,
            "Preserve file contents after reopening"
        );
    } else {
        CHECK(
            0,
            "Preserve file metadata after reopening"
        );

        CHECK(
            0,
            "Preserve file contents after reopening"
        );
    }

    /*
     * Test 28: Close the reopened disk image.
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