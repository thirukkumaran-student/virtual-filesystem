#define _POSIX_C_SOURCE 200809L

#include "vfs_dir.h"
#include "vfs_alloc.h"
#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DIRENT_HEADER_SIZE 8U
#define DIRENT_ALIGNMENT   4U

/*
 * Directory-entry field offsets.
 */
#define DIRENT_INODE_OFFSET  0U
#define DIRENT_RECLEN_OFFSET 4U
#define DIRENT_TYPE_OFFSET   6U
#define DIRENT_NAMELEN_OFFSET 7U
#define DIRENT_NAME_OFFSET   8U

/*
 * Decode and encode little-endian integers.
 */
static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)(
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8)
    );
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static void put_u16_le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Calculate the record size needed for a filename,
 * including its header and four-byte alignment.
 */
static uint32_t required_record_length(uint32_t name_length)
{
    uint32_t length = DIRENT_HEADER_SIZE + name_length;

    return
        (length + DIRENT_ALIGNMENT - 1U) &
        ~(DIRENT_ALIGNMENT - 1U);
}

/*
 * Validate a filename for lookup, insertion, or removal.
 */
static int validate_name(const char *name, uint32_t *name_length)
{
    if (name == NULL || name_length == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t length = strlen(name);

    if (length == 0U || length > VFS_NAME_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (strchr(name, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }

    *name_length = (uint32_t)length;
    return 0;
}

/*
 * Check whether the name is a reserved directory entry.
 */
static bool is_special_name(const char *name)
{
    return
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0;
}

/*
 * Validate the disk and read a directory inode.
 */
static int read_directory_inode(
    vfs_disk_t *disk,
    uint32_t inode_number,
    vfs_inode_t *directory
)
{
    if (disk == NULL ||
        !disk->is_open ||
        disk->fd < 0 ||
        directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (vfs_is_formatted(disk) != 1) {
        errno = EINVAL;
        return -1;
    }

    if (vfs_inode_read(
            disk,
            inode_number,
            directory) == -1) {
        return -1;
    }

    if ((directory->mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    if (directory->size == 0U ||
        directory->size >
            (uint64_t)VFS_INODE_DIRECT_BLOCKS * VFS_BLOCK_SIZE) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Validate one directory record.
 *
 * The record must fit entirely inside the remaining bytes
 * of the current directory block.
 */
static int validate_record(
    const uint8_t *block,
    uint32_t offset,
    uint32_t limit,
    uint32_t *record_length,
    uint32_t *name_length
)
{
    if (block == NULL ||
        record_length == NULL ||
        name_length == NULL ||
        offset > limit ||
        limit - offset < DIRENT_HEADER_SIZE) {
        errno = EIO;
        return -1;
    }

    const uint8_t *entry = block + offset;

    uint32_t rec_len = get_u16_le(
        entry + DIRENT_RECLEN_OFFSET
    );

    uint32_t name_len = entry[DIRENT_NAMELEN_OFFSET];

    if (rec_len < DIRENT_HEADER_SIZE ||
        rec_len % DIRENT_ALIGNMENT != 0U ||
        rec_len > limit - offset ||
        name_len > VFS_NAME_MAX ||
        name_len > rec_len - DIRENT_HEADER_SIZE) {
        errno = EIO;
        return -1;
    }

    /*
     * Active entries must have a valid inode number and type.
     */
    /*
 * A nonzero name length identifies an active directory entry.
 * Inode 0 is valid because it identifies the root directory.
 * Deleted entries have a name length of zero.
 */
if (name_len != 0U) {
    uint32_t entry_inode = get_u32_le(
        entry + DIRENT_INODE_OFFSET
    );

    uint8_t type = entry[DIRENT_TYPE_OFFSET];

    if (entry_inode >= VFS_INODE_COUNT) {
        errno = EIO;
        return -1;
    }

    if (type != VFS_DIRENT_REGULAR &&
        type != VFS_DIRENT_DIRECTORY) {
        errno = EIO;
        return -1;
    }
}

    *record_length = rec_len;
    *name_length = name_len;

    return 0;
}

/*
 * Compare a directory entry's name with the requested name.
 */
static bool entry_name_matches(
    const uint8_t *entry,
    uint32_t entry_name_length,
    const char *name,
    uint32_t name_length
)
{
    return
        entry_name_length != 0U &&
        entry_name_length == name_length &&
        memcmp(
            entry + DIRENT_NAME_OFFSET,
            name,
            name_length
        ) == 0;
}

/*
 * Find an entry in a directory.
 *
 * If found, optionally returns its block, offset, record length,
 * name length, and previous-entry offset.
 */
static int find_entry(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t name_length,
    uint32_t *found_inode,
    uint32_t *found_block,
    uint32_t *found_offset,
    uint32_t *found_record_length,
    uint32_t *found_name_length,
    uint32_t *previous_offset
)
{
    vfs_inode_t directory;

    if (read_directory_inode(
            disk,
            directory_inode_number,
            &directory) == -1) {
        return -1;
    }

    uint64_t remaining = directory.size;
    uint32_t previous = UINT32_MAX;

    for (uint32_t i = 0U;
         i < VFS_INODE_DIRECT_BLOCKS && remaining > 0U;
         i++) {

        uint32_t block_number = directory.direct_blocks[i];

        if (block_number == 0U) {
            errno = EIO;
            return -1;
        }

        if (block_number >= VFS_TOTAL_BLOCKS ||
            block_number < VFS_DATA_START) {
            errno = EIO;
            return -1;
        }

        uint8_t block[VFS_BLOCK_SIZE];

        if (vfs_disk_read_block(
                disk,
                block_number,
                block) == -1) {
            return -1;
        }

        uint32_t limit = remaining > VFS_BLOCK_SIZE
            ? VFS_BLOCK_SIZE
            : (uint32_t)remaining;

        uint32_t offset = 0U;

        while (offset < limit) {
            uint32_t rec_len;
            uint32_t entry_name_length;

            if (validate_record(
                    block,
                    offset,
                    limit,
                    &rec_len,
                    &entry_name_length) == -1) {
                return -1;
            }

            const uint8_t *entry = block + offset;

            if (entry_name_matches(
                    entry,
                    entry_name_length,
                    name,
                    name_length)) {

                if (found_inode != NULL) {
                    *found_inode = get_u32_le(
                        entry + DIRENT_INODE_OFFSET
                    );
                }

                if (found_block != NULL) {
                    *found_block = block_number;
                }

                if (found_offset != NULL) {
                    *found_offset = offset;
                }

                if (found_record_length != NULL) {
                    *found_record_length = rec_len;
                }

                if (found_name_length != NULL) {
                    *found_name_length = entry_name_length;
                }

                if (previous_offset != NULL) {
                    *previous_offset = previous;
                }

                return 0;
            }

            previous = offset;
            offset += rec_len;
        }

        remaining -= limit;
    }

    errno = ENOENT;
    return -1;
}

/*
 * Look up a name and return its inode number.
 */
int vfs_dir_lookup(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t *inode_number
)
{
    uint32_t name_length;

    if (inode_number == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_name(name, &name_length) == -1) {
        return -1;
    }

    return find_entry(
        disk,
        directory_inode_number,
        name,
        name_length,
        inode_number,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    );
}

/*
 * Add a directory entry to an existing directory block.
 */
static int insert_into_existing_blocks(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t name_length,
    uint32_t inode_number,
    uint8_t type
)
{
    vfs_inode_t directory;

    if (read_directory_inode(
            disk,
            directory_inode_number,
            &directory) == -1) {
        return -1;
    }

    uint32_t needed = required_record_length(name_length);
    uint64_t remaining = directory.size;

    for (uint32_t i = 0U;
         i < VFS_INODE_DIRECT_BLOCKS && remaining > 0U;
         i++) {

        uint32_t block_number = directory.direct_blocks[i];

        if (block_number == 0U ||
            block_number >= VFS_TOTAL_BLOCKS ||
            block_number < VFS_DATA_START) {
            errno = EIO;
            return -1;
        }

        uint8_t block[VFS_BLOCK_SIZE];

        if (vfs_disk_read_block(
                disk,
                block_number,
                block) == -1) {
            return -1;
        }

        uint32_t limit = remaining > VFS_BLOCK_SIZE
            ? VFS_BLOCK_SIZE
            : (uint32_t)remaining;

        uint32_t offset = 0U;

        while (offset < limit) {
            uint32_t rec_len;
            uint32_t existing_name_length;

            if (validate_record(
                    block,
                    offset,
                    limit,
                    &rec_len,
                    &existing_name_length) == -1) {
                return -1;
            }

            uint8_t *entry = block + offset;

            if (existing_name_length == 0U && rec_len >= needed) {
                /*
                 * Reuse an unused directory record.
                 */
                memset(entry, 0, rec_len);

                put_u32_le(
                    entry + DIRENT_INODE_OFFSET,
                    inode_number
                );

                put_u16_le(
                    entry + DIRENT_RECLEN_OFFSET,
                    (uint16_t)rec_len
                );

                entry[DIRENT_TYPE_OFFSET] = type;
                entry[DIRENT_NAMELEN_OFFSET] =
                    (uint8_t)name_length;

                memcpy(
                    entry + DIRENT_NAME_OFFSET,
                    name,
                    name_length
                );

                return vfs_disk_write_block(
                    disk,
                    block_number,
                    block
                );
            }

            if (existing_name_length != 0U) {
                uint32_t minimum_existing =
                    required_record_length(existing_name_length);

                if (rec_len < minimum_existing) {
                    errno = EIO;
                    return -1;
                }

                uint32_t extra = rec_len - minimum_existing;

                if (extra >= needed) {
                    /*
                     * Split the existing record.
                     */
                    put_u16_le(
                        entry + DIRENT_RECLEN_OFFSET,
                        (uint16_t)minimum_existing
                    );

                    uint8_t *new_entry =
                        entry + minimum_existing;

                    uint32_t new_rec_len = extra;

                    memset(new_entry, 0, new_rec_len);

                    put_u32_le(
                        new_entry + DIRENT_INODE_OFFSET,
                        inode_number
                    );

                    put_u16_le(
                        new_entry + DIRENT_RECLEN_OFFSET,
                        (uint16_t)new_rec_len
                    );

                    new_entry[DIRENT_TYPE_OFFSET] = type;
                    new_entry[DIRENT_NAMELEN_OFFSET] =
                        (uint8_t)name_length;

                    memcpy(
                        new_entry + DIRENT_NAME_OFFSET,
                        name,
                        name_length
                    );

                    return vfs_disk_write_block(
                        disk,
                        block_number,
                        block
                    );
                }
            }

            offset += rec_len;
        }

        remaining -= limit;
    }

    return 1;
}

/*
 * Add an entry, allocating a new direct block if necessary.
 */
int vfs_dir_add(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name,
    uint32_t inode_number,
    uint8_t type
)
{
    uint32_t name_length;

    if (validate_name(name, &name_length) == -1) {
        return -1;
    }

    if (is_special_name(name)) {
        errno = EINVAL;
        return -1;
    }

    if (type != VFS_DIRENT_REGULAR &&
        type != VFS_DIRENT_DIRECTORY) {
        errno = EINVAL;
        return -1;
    }

    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Confirm that the target inode exists and is allocated.
     */
    vfs_inode_t target_inode;

    if (vfs_inode_read(
            disk,
            inode_number,
            &target_inode) == -1) {
        return -1;
    }

    /*
     * Ensure the entry type agrees with the target inode type.
     */
    uint16_t target_type =
        target_inode.mode & 0170000U;

    if ((type == VFS_DIRENT_DIRECTORY &&
         target_type != VFS_MODE_DIRECTORY) ||
        (type == VFS_DIRENT_REGULAR &&
         target_type != VFS_MODE_REGULAR)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t existing_inode;

    if (vfs_dir_lookup(
            disk,
            directory_inode_number,
            name,
            &existing_inode) == 0) {
        errno = EEXIST;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    int result = insert_into_existing_blocks(
        disk,
        directory_inode_number,
        name,
        name_length,
        inode_number,
        type
    );

    if (result == 0) {
        return 0;
    }

    if (result == -1) {
        return -1;
    }

    /*
     * No space was found in existing directory records.
     * A new block can only be appended at a block boundary.
     */
    vfs_inode_t directory;

    if (read_directory_inode(
            disk,
            directory_inode_number,
            &directory) == -1) {
        return -1;
    }

    if (directory.size % VFS_BLOCK_SIZE != 0U) {
        errno = ENOSPC;
        return -1;
    }

    uint32_t slot = (uint32_t)(
        directory.size / VFS_BLOCK_SIZE
    );

    if (slot >= VFS_INODE_DIRECT_BLOCKS) {
        errno = ENOSPC;
        return -1;
    }

    uint32_t new_block;

    if (vfs_alloc_block(disk, &new_block) == -1) {
        return -1;
    }

    uint8_t block[VFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    put_u32_le(
        block + DIRENT_INODE_OFFSET,
        inode_number
    );

    put_u16_le(
        block + DIRENT_RECLEN_OFFSET,
        (uint16_t)VFS_BLOCK_SIZE
    );

    block[DIRENT_TYPE_OFFSET] = type;
    block[DIRENT_NAMELEN_OFFSET] = (uint8_t)name_length;

    memcpy(
        block + DIRENT_NAME_OFFSET,
        name,
        name_length
    );

    if (vfs_disk_write_block(
            disk,
            new_block,
            block) == -1) {
        int saved_errno = errno;
        (void)vfs_free_block(disk, new_block);
        errno = saved_errno;
        return -1;
    }

    directory.direct_blocks[slot] = new_block;
    directory.size += VFS_BLOCK_SIZE;

    if (vfs_inode_write(
            disk,
            directory_inode_number,
            &directory) == -1) {
        /*
         * Do not free new_block here. If the inode write partially
         * reached the image, the inode may already reference it.
         * Keeping the block allocated avoids a possible dangling
         * pointer. This path is still not transactional.
         */
        return -1;
    }

    return 0;
}

/*
 * Remove a directory entry.
 */
int vfs_dir_remove(
    vfs_disk_t *disk,
    uint32_t directory_inode_number,
    const char *name
)
{
    uint32_t name_length;

    if (validate_name(name, &name_length) == -1) {
        return -1;
    }

    if (is_special_name(name)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t inode_number;
    uint32_t block_number;
    uint32_t offset;
    uint32_t rec_len;
    uint32_t entry_name_length;
    uint32_t previous_offset;

    if (find_entry(
            disk,
            directory_inode_number,
            name,
            name_length,
            &inode_number,
            &block_number,
            &offset,
            &rec_len,
            &entry_name_length,
            &previous_offset) == -1) {
        return -1;
    }

    uint8_t block[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk,
            block_number,
            block) == -1) {
        return -1;
    }

    uint8_t *entry = block + offset;

    if (previous_offset != UINT32_MAX) {
        /*
         * Merge the removed record into its predecessor.
         */
        uint8_t *previous = block + previous_offset;

        uint32_t previous_rec_len = get_u16_le(
            previous + DIRENT_RECLEN_OFFSET
        );

        if (previous_rec_len > UINT16_MAX - rec_len) {
            errno = EIO;
            return -1;
        }

        put_u16_le(
            previous + DIRENT_RECLEN_OFFSET,
            (uint16_t)(previous_rec_len + rec_len)
        );
    } else {
        /*
         * The first record has no predecessor.
         * Mark it unused while preserving its record length.
         */
        memset(entry, 0, rec_len);

        put_u16_le(
            entry + DIRENT_RECLEN_OFFSET,
            (uint16_t)rec_len
        );
    }

    return vfs_disk_write_block(
        disk,
        block_number,
        block
    );
}
