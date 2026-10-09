
#define _POSIX_C_SOURCE 200809L

#include "vfs_check.h"
#include "vfs_file.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

enum {
    SB_FREE_BLOCKS_OFFSET = 64,
    SB_FREE_INODES_OFFSET = 68
};

static uint32_t get_u32_le(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8U) |
        ((uint32_t)buffer[2] << 16U) |
        ((uint32_t)buffer[3] << 24U);
}

static int bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8U] &
            (uint8_t)(1U << (bit % 8U))) != 0U;
}

static void report_failure(
    FILE *output,
    uint32_t *errors,
    const char *message)
{
    fprintf(output, "[FAIL] %s\n", message);
    (*errors)++;
}

int vfs_check(vfs_disk_t *disk, FILE *output)
{
    if (disk == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    fprintf(output, "\nFilesystem Consistency Check\n");
    fprintf(output, "============================\n");

    int formatted = vfs_is_formatted(disk);

    if (formatted == -1) {
        return -1;
    }

    if (formatted == 0) {
        report_failure(
            output,
            &(uint32_t){0},
            "Invalid or unrecognized filesystem superblock."
        );
        fprintf(output, "Result: INCONSISTENT\n");
        return 1;
    }

    uint8_t superblock[VFS_BLOCK_SIZE];
    uint8_t block_bitmap[VFS_BLOCK_SIZE];
    uint8_t inode_bitmap[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk, VFS_SUPERBLOCK_BLOCK, superblock) == -1 ||
        vfs_disk_read_block(
            disk, VFS_BLOCK_BITMAP_BLOCK, block_bitmap) == -1 ||
        vfs_disk_read_block(
            disk, VFS_INODE_BITMAP_BLOCK, inode_bitmap) == -1) {
        return -1;
    }

    uint32_t errors = 0U;
    uint32_t stored_free_blocks =
        get_u32_le(superblock + SB_FREE_BLOCKS_OFFSET);
    uint32_t stored_free_inodes =
        get_u32_le(superblock + SB_FREE_INODES_OFFSET);

    /*
     * Every block before the allocatable data region is reserved.
     * Block VFS_DATA_START belongs to the root directory.
     */
    for (uint32_t block = 0U; block <= VFS_DATA_START; block++) {
        if (!bitmap_test(block_bitmap, block)) {
            report_failure(
                output,
                &errors,
                "A reserved block is marked free."
            );
            break;
        }
    }

    if (!bitmap_test(inode_bitmap, VFS_ROOT_INODE)) {
        report_failure(
            output,
            &errors,
            "The root inode is marked free."
        );
    }

    uint32_t counted_free_blocks = 0U;

    for (uint32_t block = VFS_DATA_START + 1U;
         block < VFS_TOTAL_BLOCKS;
         block++) {
        if (!bitmap_test(block_bitmap, block)) {
            counted_free_blocks++;
        }
    }

    if (counted_free_blocks != stored_free_blocks) {
        fprintf(
            output,
            "[FAIL] Free-block counter mismatch: stored=%" PRIu32
            ", counted=%" PRIu32 ".\n",
            stored_free_blocks,
            counted_free_blocks
        );
        errors++;
    }

    uint32_t counted_free_inodes = 0U;

    for (uint32_t inode = 1U; inode < VFS_INODE_COUNT; inode++) {
        if (!bitmap_test(inode_bitmap, inode)) {
            counted_free_inodes++;
        }
    }

    if (counted_free_inodes != stored_free_inodes) {
        fprintf(
            output,
            "[FAIL] Free-inode counter mismatch: stored=%" PRIu32
            ", counted=%" PRIu32 ".\n",
            stored_free_inodes,
            counted_free_inodes
        );
        errors++;
    }

    /*
     * Track which inode references each data block.
     * -1 means no inode has referenced that block.
     */
    int32_t block_owner[VFS_TOTAL_BLOCKS];

    for (uint32_t block = 0U; block < VFS_TOTAL_BLOCKS; block++) {
        block_owner[block] = -1;
    }

    block_owner[VFS_DATA_START] = (int32_t)VFS_ROOT_INODE;

    for (uint32_t inode_number = 0U;
         inode_number < VFS_INODE_COUNT;
         inode_number++) {
        if (!bitmap_test(inode_bitmap, inode_number)) {
            continue;
        }

        vfs_inode_t inode;

        if (vfs_inode_read(
                disk, inode_number, &inode) == -1) {
            return -1;
        }

        uint16_t type = (uint16_t)(inode.mode & 0170000U);

        if (type != VFS_MODE_DIRECTORY &&
            type != VFS_MODE_REGULAR) {
            report_failure(
                output,
                &errors,
                "An allocated inode has an unsupported file type."
            );
            continue;
        }

        if (inode.indirect_block != 0U) {
            report_failure(
                output,
                &errors,
                "Indirect-block pointers are unsupported by this checker."
            );
        }

        if (type == VFS_MODE_DIRECTORY &&
            (inode.size == 0U ||
             inode.size % VFS_BLOCK_SIZE != 0U)) {
            report_failure(
                output,
                &errors,
                "A directory has an invalid size."
            );
        }

        if (type == VFS_MODE_REGULAR &&
            inode.size > VFS_FILE_MAX_SIZE) {
            report_failure(
                output,
                &errors,
                "A regular file exceeds the supported direct-block size."
            );
        }

        uint64_t required_blocks =
            (inode.size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE;

        if (required_blocks > VFS_INODE_DIRECT_BLOCKS) {
            report_failure(
                output,
                &errors,
                "An inode requires more blocks than its direct pointers support."
            );
            continue;
        }

        if (inode_number == VFS_ROOT_INODE &&
            (type != VFS_MODE_DIRECTORY ||
             inode.direct_blocks[0] != VFS_DATA_START)) {
            report_failure(
                output,
                &errors,
                "The root inode does not reference the expected root directory block."
            );
        }

        for (uint32_t index = 0U;
             index < VFS_INODE_DIRECT_BLOCKS;
             index++) {
            uint32_t block = inode.direct_blocks[index];

            if (index < required_blocks && block == 0U) {
                report_failure(
                    output,
                    &errors,
                    "An inode is missing a required direct block."
                );
                continue;
            }

            if (index >= required_blocks && block != 0U) {
                report_failure(
                    output,
                    &errors,
                    "An inode has an unexpected direct block beyond its size."
                );
            }

            if (block == 0U) {
                continue;
            }

            if (block < VFS_DATA_START ||
                block >= VFS_TOTAL_BLOCKS) {
                report_failure(
                    output,
                    &errors,
                    "An inode contains an out-of-range data-block pointer."
                );
                continue;
            }

            if (block == VFS_DATA_START &&
                inode_number != VFS_ROOT_INODE) {
                report_failure(
                    output,
                    &errors,
                    "A non-root inode references the reserved root directory block."
                );
                continue;
            }

            if (!bitmap_test(block_bitmap, block)) {
                report_failure(
                    output,
                    &errors,
                    "An inode references a block marked free."
                );
            }

            
            bool root_block_reference =
                block == VFS_DATA_START &&
                inode_number == VFS_ROOT_INODE;

            if (block_owner[block] != -1 &&
                !root_block_reference) {
                report_failure(
                    output,
                    &errors,
                    "A data block is referenced more than once."
                );
            } else if (block_owner[block] == -1) {
                block_owner[block] = (int32_t)inode_number;
            }

        }
    }

    for (uint32_t block = VFS_DATA_START + 1U;
         block < VFS_TOTAL_BLOCKS;
         block++) {
        if (bitmap_test(block_bitmap, block) &&
            block_owner[block] == -1) {
            report_failure(
                output,
                &errors,
                "An allocated data block is not referenced by an inode."
            );
        }
    }

    if (errors == 0U) {
        fprintf(output, "[PASS] All implemented consistency checks passed.\n");
        fprintf(output, "Result: CONSISTENT\n");
        return 0;
    }

    fprintf(
        output,
        "Detected issues: %" PRIu32 "\n",
        errors
    );
    fprintf(output, "Result: INCONSISTENT\n");

    return 1;
}
