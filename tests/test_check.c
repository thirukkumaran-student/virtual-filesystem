#define _POSIX_C_SOURCE 200809L

#include "vfs_check.h"
#include "vfs_dir.h"
#include "vfs_disk.h"
#include "vfs_file.h"
#include "vfs_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_check.img"
#define DIRENT_HEADER_SIZE 8U
#define DIRENT_INODE_OFFSET 0U
#define DIRENT_RECLEN_OFFSET 4U
#define DIRENT_TYPE_OFFSET 6U
#define DIRENT_NAMELEN_OFFSET 7U
#define DIRENT_NAME_OFFSET 8U

static int tests_passed;
static int tests_failed;

#define CHECK(condition, description)                                \
    do {                                                             \
        if (condition) {                                             \
            printf("[PASS] %s\n", description);                     \
            tests_passed++;                                          \
        } else {                                                     \
            printf("[FAIL] %s (line %d)\n", description, __LINE__); \
            tests_failed++;                                          \
        }                                                            \
    } while (0)

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

// static uint32_t get_u32_le(const uint8_t *p)
// {
//     return (uint32_t)p[0] |
//            ((uint32_t)p[1] << 8U) |
//            ((uint32_t)p[2] << 16U) |
//            ((uint32_t)p[3] << 24U);
// }

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)((value >> 8U) & 0xffU);
    p[2] = (uint8_t)((value >> 16U) & 0xffU);
    p[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static int find_entry_offset(
    const uint8_t *block,
    const char *name,
    uint32_t *entry_offset)
{
    const size_t name_length = strlen(name);
    uint32_t offset = 0U;

    while (offset + DIRENT_HEADER_SIZE <= VFS_BLOCK_SIZE) {
        uint32_t record_length = get_u16_le(
            block + offset + DIRENT_RECLEN_OFFSET);
        uint32_t stored_name_length = block[offset + DIRENT_NAMELEN_OFFSET];

        if (record_length < DIRENT_HEADER_SIZE ||
            record_length % 4U != 0U ||
            record_length > VFS_BLOCK_SIZE - offset ||
            stored_name_length > record_length - DIRENT_HEADER_SIZE) {
            return -1;
        }

        if (stored_name_length == name_length &&
            memcmp(block + offset + DIRENT_NAME_OFFSET,
                   name, name_length) == 0) {
            *entry_offset = offset;
            return 0;
        }

        offset += record_length;
    }

    return -1;
}

static int find_free_inode(vfs_disk_t *disk, uint32_t *inode_number)
{
    uint8_t bitmap[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, VFS_INODE_BITMAP_BLOCK, bitmap) == -1) {
        return -1;
    }

    for (uint32_t inode = 1U; inode < VFS_INODE_COUNT; inode++) {
        if ((bitmap[inode / 8U] & (uint8_t)(1U << (inode % 8U))) == 0U) {
            *inode_number = inode;
            return 0;
        }
    }

    return -1;
}

static int check_is_inconsistent(vfs_disk_t *disk)
{
    return vfs_check(disk, stdout) == 1;
}

int main(void)
{
    vfs_disk_t disk = {.fd = -1, .is_open = false};
    uint8_t original_block[VFS_BLOCK_SIZE];
    uint8_t changed_block[VFS_BLOCK_SIZE];
    uint8_t observed_block[VFS_BLOCK_SIZE];
    uint32_t file_inode = UINT32_MAX;
    uint32_t entry_offset = UINT32_MAX;
    int result;

    (void)unlink(TEST_IMAGE);
    (void)mkdir("build", 0755);
    printf("=== Filesystem Consistency Checker Tests ===\n\n");

    result = vfs_disk_create(TEST_IMAGE);
    CHECK(result == 0, "Create a disk image");
    if (result != 0) {
        return EXIT_FAILURE;
    }

    result = vfs_disk_open(&disk, TEST_IMAGE);
    CHECK(result == 0, "Open the disk image");
    if (result != 0) {
        (void)unlink(TEST_IMAGE);
        return EXIT_FAILURE;
    }

    result = vfs_format(&disk);
    CHECK(result == 0, "Format the filesystem");

    result = vfs_file_create(&disk, VFS_ROOT_INODE, "probe.txt", &file_inode);
    CHECK(result == 0, "Create a file to use in corruption tests");

    result = vfs_disk_read_block(&disk, VFS_DATA_START, original_block);
    CHECK(result == 0, "Read the root directory block");

    result = find_entry_offset(original_block, "probe.txt", &entry_offset);
    CHECK(result == 0, "Locate the test file's directory entry");

    result = vfs_check(&disk, stdout);
    CHECK(result == 0, "Accept an unmodified filesystem");

    if (result == 0 && entry_offset != UINT32_MAX) {
        uint32_t free_inode = UINT32_MAX;
        result = find_free_inode(&disk, &free_inode);
        CHECK(result == 0, "Find an inode that is marked free");

        if (result == 0) {
            memcpy(changed_block, original_block, sizeof changed_block);
            put_u32_le(changed_block + entry_offset + DIRENT_INODE_OFFSET,
                       free_inode);
            result = vfs_disk_write_block(&disk, VFS_DATA_START, changed_block);
            CHECK(result == 0, "Inject a reference to an unallocated inode");
            CHECK(check_is_inconsistent(&disk),
                  "Detect a directory entry referencing a free inode");
            result = vfs_disk_read_block(&disk, VFS_DATA_START, observed_block);
            CHECK(result == 0 && memcmp(observed_block, changed_block,
                                         sizeof changed_block) == 0,
                  "Checker leaves the corrupted directory block unchanged");
            (void)vfs_disk_write_block(&disk, VFS_DATA_START, original_block);
        }

        memcpy(changed_block, original_block, sizeof changed_block);
        put_u32_le(changed_block + entry_offset + DIRENT_INODE_OFFSET,
                   VFS_INODE_COUNT);
        result = vfs_disk_write_block(&disk, VFS_DATA_START, changed_block);
        CHECK(result == 0, "Inject an out-of-range inode number");
        CHECK(check_is_inconsistent(&disk),
              "Detect a directory entry with an out-of-range inode");
        (void)vfs_disk_write_block(&disk, VFS_DATA_START, original_block);

        memcpy(changed_block, original_block, sizeof changed_block);
        changed_block[entry_offset + DIRENT_RECLEN_OFFSET] = 0U;
        changed_block[entry_offset + DIRENT_RECLEN_OFFSET + 1U] = 0U;
        result = vfs_disk_write_block(&disk, VFS_DATA_START, changed_block);
        CHECK(result == 0, "Inject a malformed directory record length");
        CHECK(check_is_inconsistent(&disk),
              "Detect a malformed directory record");
        (void)vfs_disk_write_block(&disk, VFS_DATA_START, original_block);

        memcpy(changed_block, original_block, sizeof changed_block);
        changed_block[entry_offset + DIRENT_NAMELEN_OFFSET] = 255U;
        result = vfs_disk_write_block(&disk, VFS_DATA_START, changed_block);
        CHECK(result == 0, "Inject an invalid directory filename length");
        CHECK(check_is_inconsistent(&disk),
              "Detect a directory filename length that exceeds its record");
        (void)vfs_disk_write_block(&disk, VFS_DATA_START, original_block);

        memcpy(changed_block, original_block, sizeof changed_block);
        changed_block[entry_offset + DIRENT_TYPE_OFFSET] = VFS_DIRENT_DIRECTORY;
        result = vfs_disk_write_block(&disk, VFS_DATA_START, changed_block);
        CHECK(result == 0, "Inject a directory-entry type mismatch");
        CHECK(check_is_inconsistent(&disk),
              "Detect a directory-entry type that disagrees with its inode");
        (void)vfs_disk_write_block(&disk, VFS_DATA_START, original_block);

        result = vfs_check(&disk, stdout);
        CHECK(result == 0, "Remain consistent after restoring directory metadata");
    } else {
        CHECK(0, "Find an inode that is marked free");
        CHECK(0, "Inject a reference to an unallocated inode");
        CHECK(0, "Detect a directory entry referencing a free inode");
        CHECK(0, "Checker leaves the corrupted directory block unchanged");
        CHECK(0, "Inject an out-of-range inode number");
        CHECK(0, "Detect a directory entry with an out-of-range inode");
        CHECK(0, "Inject a malformed directory record length");
        CHECK(0, "Detect a malformed directory record");
        CHECK(0, "Inject an invalid directory filename length");
        CHECK(0, "Detect a directory filename length that exceeds its record");
        CHECK(0, "Inject a directory-entry type mismatch");
        CHECK(0, "Detect a directory-entry type that disagrees with its inode");
        CHECK(0, "Remain consistent after restoring directory metadata");
    }

    result = vfs_disk_close(&disk);
    CHECK(result == 0, "Close the disk image");
    (void)unlink(TEST_IMAGE);

    printf("\n=== Test Summary ===\nPassed: %d\nFailed: %d\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
