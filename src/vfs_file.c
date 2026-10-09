#define _POSIX_C_SOURCE 200809L

#include "vfs_file.h"

#include "vfs_alloc.h"
#include "vfs_dir.h"
#include "vfs_format.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * Return the current Unix timestamp.
 */
static uint64_t current_timestamp(void)
{
    time_t now = time(NULL);

    return now < (time_t)0 ? 0U : (uint64_t)now;
}

/*
 * Validate an open, formatted disk image.
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
 * Read an inode and ensure it represents a regular file.
 */
static int read_regular_inode(
    vfs_disk_t *disk,
    uint32_t inode_number,
    vfs_inode_t *inode
)
{
    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (vfs_inode_read(disk, inode_number, inode) == -1) {
        return -1;
    }

    if ((inode->mode & 0170000U) != VFS_MODE_REGULAR) {
        errno = EISDIR;
        return -1;
    }

    if (inode->size > VFS_FILE_MAX_SIZE) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Validate a data-block pointer before using it.
 */
static int validate_data_block(uint32_t block_number)
{
    if (block_number < VFS_DATA_START ||
        block_number >= VFS_TOTAL_BLOCKS) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Create a regular file and link it into its parent directory.
 */
int vfs_file_create(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name,
    uint32_t *inode_number
)
{
    if (disk == NULL ||
        !disk->is_open ||
        disk->fd < 0 ||
        name == NULL ||
        inode_number == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_disk(disk) == -1) {
        return -1;
    }

    uint32_t allocated_inode;

    if (vfs_alloc_inode(disk, &allocated_inode) == -1) {
        return -1;
    }

    uint64_t timestamp = current_timestamp();

    vfs_inode_t inode;
    memset(&inode, 0, sizeof(inode));

    inode.mode = (uint16_t)(VFS_MODE_REGULAR | 0644U);
    inode.uid = 0U;
    inode.gid = 0U;
    inode.links = 1U;
    inode.size = 0U;
    inode.atime = timestamp;
    inode.mtime = timestamp;
    inode.ctime = timestamp;

    if (vfs_inode_write(
            disk,
            allocated_inode,
            &inode) == -1) {
        int saved_errno = errno;
        (void)vfs_free_inode(disk, allocated_inode);
        errno = saved_errno;
        return -1;
    }

    if (vfs_dir_add(
            disk,
            parent_inode_number,
            name,
            allocated_inode,
            VFS_DIRENT_REGULAR) == -1) {
        int saved_errno = errno;

        /*
         * The directory entry was not created. Clear the inode
         * before returning it to the free-inode pool.
         */
        memset(&inode, 0, sizeof(inode));

        if (vfs_inode_write(
                disk,
                allocated_inode,
                &inode) == 0) {
            (void)vfs_free_inode(disk, allocated_inode);
        }

        errno = saved_errno;
        return -1;
    }

    *inode_number = allocated_inode;
    return 0;
}

/*
 * Read bytes from a regular file.
 */
int64_t vfs_file_read(
    vfs_disk_t *disk,
    uint32_t inode_number,
    void *buffer,
    size_t count,
    uint64_t offset
)
{
    if (count > 0U && buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (count > (size_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }

    vfs_inode_t inode;

    if (read_regular_inode(
            disk,
            inode_number,
            &inode) == -1) {
        return -1;
    }

    if (count == 0U || offset >= inode.size) {
        return 0;
    }

    uint64_t available = inode.size - offset;

    size_t bytes_to_read = count;

    if ((uint64_t)bytes_to_read > available) {
        bytes_to_read = (size_t)available;
    }

    uint8_t *output = buffer;
    size_t total_read = 0U;

    while (total_read < bytes_to_read) {
        uint64_t position = offset + total_read;

        uint32_t block_index =
            (uint32_t)(position / VFS_BLOCK_SIZE);

        uint32_t block_offset =
            (uint32_t)(position % VFS_BLOCK_SIZE);

        if (block_index >= VFS_INODE_DIRECT_BLOCKS) {
            errno = EIO;
            return -1;
        }

        uint32_t block_number = inode.direct_blocks[block_index];

        if (validate_data_block(block_number) == -1) {
            return -1;
        }

        uint8_t block[VFS_BLOCK_SIZE];

        if (vfs_disk_read_block(
                disk,
                block_number,
                block) == -1) {
            return -1;
        }

        size_t chunk = VFS_BLOCK_SIZE - block_offset;

        if (chunk > bytes_to_read - total_read) {
            chunk = bytes_to_read - total_read;
        }

        memcpy(
            output + total_read,
            block + block_offset,
            chunk
        );

        total_read += chunk;
    }

    inode.atime = current_timestamp();

    if (vfs_inode_write(disk, inode_number, &inode) == -1) {
        return -1;
    }

    return (int64_t)total_read;
}

/*
 * Write bytes to a regular file.
 *
 * Sparse writes are rejected: offset must not exceed the current
 * file size. Blocks are zero-initialized when first allocated.
 */
int64_t vfs_file_write(
    vfs_disk_t *disk,
    uint32_t inode_number,
    const void *buffer,
    size_t count,
    uint64_t offset
)
{
    if (count > 0U && buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (count > (size_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0U) {
        vfs_inode_t inode;

        if (read_regular_inode(
                disk,
                inode_number,
                &inode) == -1) {
            return -1;
        }

        return 0;
    }

    if (offset > UINT64_MAX - (uint64_t)count) {
        errno = EFBIG;
        return -1;
    }

    uint64_t end_offset = offset + (uint64_t)count;

    if (end_offset > VFS_FILE_MAX_SIZE) {
        errno = EFBIG;
        return -1;
    }

    vfs_inode_t inode;

    if (read_regular_inode(
            disk,
            inode_number,
            &inode) == -1) {
        return -1;
    }

    if (offset > inode.size) {
        errno = EINVAL;
        return -1;
    }

    uint32_t old_blocks = (uint32_t)(
        (inode.size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE
    );

    uint32_t required_blocks = (uint32_t)(
        (end_offset + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE
    );

    uint32_t newly_allocated[VFS_INODE_DIRECT_BLOCKS];
    uint32_t newly_allocated_count = 0U;

    /*
     * Allocate and zero-initialize any blocks required by the write.
     */
    for (uint32_t i = old_blocks; i < required_blocks; i++) {
        uint32_t new_block;

        if (vfs_alloc_block(disk, &new_block) == -1) {
            goto rollback_allocations;
        }

        uint8_t zero_block[VFS_BLOCK_SIZE];
        memset(zero_block, 0, sizeof(zero_block));

        if (vfs_disk_write_block(
                disk,
                new_block,
                zero_block) == -1) {
            int saved_errno = errno;
            (void)vfs_free_block(disk, new_block);
            errno = saved_errno;
            goto rollback_allocations;
        }

        inode.direct_blocks[i] = new_block;
        newly_allocated[newly_allocated_count++] = new_block;
    }

    /*
     * Perform the data writes.
     */
    {
        const uint8_t *input = buffer;
        size_t total_written = 0U;

        while (total_written < count) {
            uint64_t position = offset + total_written;

            uint32_t block_index =
                (uint32_t)(position / VFS_BLOCK_SIZE);

            uint32_t block_offset =
                (uint32_t)(position % VFS_BLOCK_SIZE);

            uint32_t block_number = inode.direct_blocks[block_index];

            if (validate_data_block(block_number) == -1) {
                goto rollback_allocations;
            }

            uint8_t block[VFS_BLOCK_SIZE];

            if (vfs_disk_read_block(
                    disk,
                    block_number,
                    block) == -1) {
                goto rollback_allocations;
            }

            size_t chunk = VFS_BLOCK_SIZE - block_offset;

            if (chunk > count - total_written) {
                chunk = count - total_written;
            }

            memcpy(
                block + block_offset,
                input + total_written,
                chunk
            );

            if (vfs_disk_write_block(
                    disk,
                    block_number,
                    block) == -1) {
                goto rollback_allocations;
            }

            total_written += chunk;
        }
    }

    if (end_offset > inode.size) {
        inode.size = end_offset;
    }

    inode.mtime = current_timestamp();
    inode.ctime = inode.mtime;

    if (vfs_inode_write(
            disk,
            inode_number,
            &inode) == -1) {
        goto rollback_allocations;
    }

    return (int64_t)count;

rollback_allocations:
    {
        int saved_errno = errno;

        /*
         * Roll back blocks allocated during this operation.
         * This is best-effort rollback, not a crash-safe transaction.
         */
        for (uint32_t i = 0U; i < newly_allocated_count; i++) {
            (void)vfs_free_block(disk, newly_allocated[i]);
        }

        errno = saved_errno;
        return -1;
    }
}

/*
 * Truncate a regular file.
 */
int vfs_file_truncate(
    vfs_disk_t *disk,
    uint32_t inode_number,
    uint64_t new_size
)
{
    if (new_size > VFS_FILE_MAX_SIZE) {
        errno = EFBIG;
        return -1;
    }

    vfs_inode_t inode;

    if (read_regular_inode(
            disk,
            inode_number,
            &inode) == -1) {
        return -1;
    }

    if (new_size == inode.size) {
        return 0;
    }

    if (new_size > inode.size) {
        /*
         * Growing the file must zero-fill the newly exposed region.
         */
        uint64_t extension_size = new_size - inode.size;

        if (extension_size > SIZE_MAX) {
            errno = EFBIG;
            return -1;
        }

        size_t bytes_to_write = (size_t)extension_size;

        /*
         * The maximum file size is only 32768 bytes, so a fixed-size
         * zero buffer is sufficient for this direct-block version.
         */
        uint8_t zeros[VFS_FILE_MAX_SIZE];
        memset(zeros, 0, sizeof(zeros));

        int64_t written = vfs_file_write(
            disk,
            inode_number,
            zeros,
            bytes_to_write,
            inode.size
        );

        if (written < 0) {
            return -1;
        }

        if ((uint64_t)written != extension_size) {
            errno = EIO;
            return -1;
        }

        return 0;
    }

    /*
     * Shrink the file.
     */
    uint32_t required_blocks = (uint32_t)(
        (new_size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE
    );

    /*
     * If the new EOF falls inside an allocated block, zero the
     * discarded tail so that later file growth cannot expose stale
     * bytes from the old contents.
     */
    if (new_size > 0U &&
        new_size % VFS_BLOCK_SIZE != 0U) {

        uint32_t block_index =
            (uint32_t)(new_size / VFS_BLOCK_SIZE);

        uint32_t block_number = inode.direct_blocks[block_index];

        if (validate_data_block(block_number) == -1) {
            return -1;
        }

        uint8_t block[VFS_BLOCK_SIZE];

        if (vfs_disk_read_block(
                disk,
                block_number,
                block) == -1) {
            return -1;
        }

        uint32_t tail_offset =
            (uint32_t)(new_size % VFS_BLOCK_SIZE);

        memset(
            block + tail_offset,
            0,
            VFS_BLOCK_SIZE - tail_offset
        );

        if (vfs_disk_write_block(
                disk,
                block_number,
                block) == -1) {
            return -1;
        }
    }

    /*
     * Release blocks beyond the new EOF.
     */
    for (uint32_t i = required_blocks;
         i < VFS_INODE_DIRECT_BLOCKS;
         i++) {

        uint32_t block_number = inode.direct_blocks[i];

        if (block_number == 0U) {
            continue;
        }

        if (validate_data_block(block_number) == -1) {
            return -1;
        }

        if (vfs_free_block(disk, block_number) == -1) {
            return -1;
        }

        inode.direct_blocks[i] = 0U;
    }

    inode.size = new_size;
    inode.mtime = current_timestamp();
    inode.ctime = inode.mtime;

    return vfs_inode_write(
        disk,
        inode_number,
        &inode
    );
}