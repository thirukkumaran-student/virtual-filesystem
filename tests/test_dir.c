#define _POSIX_C_SOURCE 200809L

#include "vfs_alloc.h"
#include "vfs_dir.h"
#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_dir.img"

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
 * Create a minimal allocated regular-file inode.
 */
static int create_regular_inode(
    vfs_disk_t *disk,
    uint32_t *inode_number
)
{
    vfs_inode_t inode;

    if (vfs_alloc_inode(disk, inode_number) == -1) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));

    inode.mode = (uint16_t)(VFS_MODE_REGULAR | 0644U);
    inode.uid = 100U;
    inode.gid = 100U;
    inode.links = 1U;
    inode.size = 0U;

    return vfs_inode_write(
        disk,
        *inode_number,
        &inode
    );
}

/*
 * Create a minimal allocated directory inode.
 */
static int create_directory_inode(
    vfs_disk_t *disk,
    uint32_t *inode_number
)
{
    vfs_inode_t inode;
    uint32_t block_number;

    if (vfs_alloc_inode(disk, inode_number) == -1) {
        return -1;
    }

    if (vfs_alloc_block(disk, &block_number) == -1) {
        return -1;
    }

    uint8_t block[VFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    /*
     * Initialize "." and ".." as entries pointing to this
     * directory. The test does not exercise parent traversal.
     */
    block[0] = (uint8_t)(*inode_number & 0xFFU);
    block[1] = (uint8_t)((*inode_number >> 8) & 0xFFU);
    block[2] = (uint8_t)((*inode_number >> 16) & 0xFFU);
    block[3] = (uint8_t)((*inode_number >> 24) & 0xFFU);

    block[4] = 12U;
    block[5] = 0U;
    block[6] = VFS_DIRENT_DIRECTORY;
    block[7] = 1U;
    block[8] = '.';

    block[12] = (uint8_t)(*inode_number & 0xFFU);
    block[13] = (uint8_t)((*inode_number >> 8) & 0xFFU);
    block[14] = (uint8_t)((*inode_number >> 16) & 0xFFU);
    block[15] = (uint8_t)((*inode_number >> 24) & 0xFFU);

    block[16] = (uint8_t)(VFS_BLOCK_SIZE - 12U);
    block[17] = (uint8_t)((VFS_BLOCK_SIZE - 12U) >> 8);
    block[18] = VFS_DIRENT_DIRECTORY;
    block[19] = 2U;
    block[20] = '.';
    block[21] = '.';

    if (vfs_disk_write_block(
            disk,
            block_number,
            block) == -1) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));

    inode.mode = (uint16_t)(VFS_MODE_DIRECTORY | 0755U);
    inode.uid = 100U;
    inode.gid = 100U;
    inode.links = 2U;
    inode.size = VFS_BLOCK_SIZE;
    inode.direct_blocks[0] = block_number;

    return vfs_inode_write(
        disk,
        *inode_number,
        &inode
    );
}

int main(void)
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    uint32_t root_inode = VFS_ROOT_INODE;
    uint32_t file_inode = UINT32_MAX;
    uint32_t directory_inode = UINT32_MAX;
    uint32_t found_inode = UINT32_MAX;

    int result;

    (void)unlink(TEST_IMAGE);

    printf("=== Directory Management Tests ===\n\n");

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
     * Test 4: Look up the root directory's "." entry.
     */
    result = vfs_dir_lookup(
        &disk,
        root_inode,
        ".",
        &found_inode
    );

    CHECK(
        result == 0 && found_inode == root_inode,
        "Look up the current-directory entry"
    );

    /*
     * Test 5: Look up the root directory's ".." entry.
     */
    found_inode = UINT32_MAX;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "..",
        &found_inode
    );

    CHECK(
        result == 0 && found_inode == root_inode,
        "Look up the parent-directory entry"
    );

    /*
     * Test 6: Reject a missing filename.
     */
    errno = 0;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "missing.txt",
        &found_inode
    );

    CHECK(
        result == -1 && errno == ENOENT,
        "Reject lookup of a nonexistent name"
    );

    /*
     * Test 7: Reject a NULL lookup output pointer.
     */
    errno = 0;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "missing.txt",
        NULL
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a NULL lookup output pointer"
    );

    /*
     * Test 8: Reject an empty filename.
     */
    errno = 0;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "",
        &found_inode
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject an empty filename"
    );

    /*
     * Test 9: Reject a filename containing a slash.
     */
    errno = 0;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "docs/file.txt",
        &found_inode
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a filename containing a slash"
    );

    /*
     * Test 10: Create an allocated regular-file inode.
     */
    result = create_regular_inode(
        &disk,
        &file_inode
    );

    CHECK(
        result == 0,
        "Create a regular-file inode"
    );

    /*
     * Test 11: Insert a regular-file entry.
     */
    result = vfs_dir_add(
        &disk,
        root_inode,
        "alpha.txt",
        file_inode,
        VFS_DIRENT_REGULAR
    );

    CHECK(
        result == 0,
        "Insert a regular-file directory entry"
    );

    /*
     * Test 12: Look up the inserted file.
     */
    found_inode = UINT32_MAX;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "alpha.txt",
        &found_inode
    );

    CHECK(
        result == 0 && found_inode == file_inode,
        "Look up an inserted file"
    );

    /*
     * Test 13: Reject duplicate names.
     */
    errno = 0;

    result = vfs_dir_add(
        &disk,
        root_inode,
        "alpha.txt",
        file_inode,
        VFS_DIRENT_REGULAR
    );

    CHECK(
        result == -1 && errno == EEXIST,
        "Reject a duplicate filename"
    );

    /*
     * Test 14: Reject attempts to insert ".".
     */
    errno = 0;

    result = vfs_dir_add(
        &disk,
        root_inode,
        ".",
        file_inode,
        VFS_DIRENT_REGULAR
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject insertion of a special directory name"
    );

    /*
     * Test 15: Reject a nonexistent target inode.
     */
    errno = 0;

    result = vfs_dir_add(
        &disk,
        root_inode,
        "ghost.txt",
        VFS_INODE_COUNT,
        VFS_DIRENT_REGULAR
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject an out-of-range target inode"
    );

    /*
     * Test 16: Create a directory inode.
     */
    result = create_directory_inode(
        &disk,
        &directory_inode
    );

    CHECK(
        result == 0,
        "Create a directory inode"
    );

    /*
     * Test 17: Insert a directory entry.
     */
    result = vfs_dir_add(
        &disk,
        root_inode,
        "documents",
        directory_inode,
        VFS_DIRENT_DIRECTORY
    );

    CHECK(
        result == 0,
        "Insert a subdirectory entry"
    );

    /*
     * Test 18: Look up the inserted subdirectory.
     */
    found_inode = UINT32_MAX;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "documents",
        &found_inode
    );

    CHECK(
        result == 0 && found_inode == directory_inode,
        "Look up an inserted subdirectory"
    );

    /*
     * Test 19: Reject a type that does not match the inode.
     */
    errno = 0;

    result = vfs_dir_add(
        &disk,
        root_inode,
        "wrong-type",
        file_inode,
        VFS_DIRENT_DIRECTORY
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject a directory-entry type mismatch"
    );

    /*
     * Test 20: Remove the regular-file entry.
     */
    result = vfs_dir_remove(
        &disk,
        root_inode,
        "alpha.txt"
    );

    CHECK(
        result == 0,
        "Remove a regular-file directory entry"
    );

    /*
     * Test 21: Verify that the removed entry is no longer found.
     */
    errno = 0;

    result = vfs_dir_lookup(
        &disk,
        root_inode,
        "alpha.txt",
        &found_inode
    );

    CHECK(
        result == -1 && errno == ENOENT,
        "Verify removal of the directory entry"
    );

    /*
     * Test 22: Reject removal of ".".
     */
    errno = 0;

    result = vfs_dir_remove(
        &disk,
        root_inode,
        "."
    );

    CHECK(
        result == -1 && errno == EINVAL,
        "Reject removal of the current-directory entry"
    );

    /*
     * Test 23: Reject removal of a missing entry.
     */
    errno = 0;

    result = vfs_dir_remove(
        &disk,
        root_inode,
        "does-not-exist"
    );

    CHECK(
        result == -1 && errno == ENOENT,
        "Reject removal of a nonexistent entry"
    );

    /*
     * Test 24: Synchronize directory changes.
     */
    result = vfs_disk_sync(&disk);

    CHECK(
        result == 0,
        "Synchronize directory changes"
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
     * Test 27: Verify that the subdirectory entry persists.
     */
    if (result == 0) {
        found_inode = UINT32_MAX;

        result = vfs_dir_lookup(
            &disk,
            root_inode,
            "documents",
            &found_inode
        );

        CHECK(
            result == 0 && found_inode == directory_inode,
            "Preserve directory entries after reopening"
        );
    } else {
        CHECK(
            0,
            "Preserve directory entries after reopening"
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