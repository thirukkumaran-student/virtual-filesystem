#define _POSIX_C_SOURCE 200809L

#include "vfs_file.h"
#include "vfs_alloc.h"
#include "vfs_dir.h"
#include "vfs_format.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PTRS_PER_INDIRECT (VFS_BLOCK_SIZE / 4U)

static uint64_t current_timestamp(void)
{
    time_t now = time(NULL);
    return now < (time_t)0 ? 0U : (uint64_t)now;
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8U) & 0xffU);
    p[2] = (uint8_t)((v >> 16U) & 0xffU);
    p[3] = (uint8_t)((v >> 24U) & 0xffU);
}

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

static int read_regular_inode(vfs_disk_t *disk, uint32_t ino,
                              vfs_inode_t *inode)
{
    if (inode == NULL || validate_disk(disk) == -1) {
        if (inode == NULL) errno = EINVAL;
        return -1;
    }
    if (vfs_inode_read(disk, ino, inode) == -1) return -1;
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

static int validate_data_block(uint32_t block)
{
    if (block < VFS_DATA_START || block >= VFS_TOTAL_BLOCKS) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static uint32_t pointer_get(const uint8_t *table, uint32_t index)
{
    return get_u32_le(table + (size_t)index * 4U);
}

static void pointer_set(uint8_t *table, uint32_t index, uint32_t block)
{
    put_u32_le(table + (size_t)index * 4U, block);
}

/* logical_index is a file-data block index, not a physical block number. */
static int get_data_block(const vfs_inode_t *inode,
                          const uint8_t *indirect_table,
                          uint32_t logical_index, uint32_t *physical)
{
    if (logical_index >= VFS_FILE_MAX_BLOCKS || physical == NULL) {
        errno = EFBIG;
        return -1;
    }
    if (logical_index < VFS_INODE_DIRECT_BLOCKS) {
        *physical = inode->direct_blocks[logical_index];
    } else {
        if (inode->indirect_block == 0U || indirect_table == NULL) {
            errno = EIO;
            return -1;
        }
        *physical = pointer_get(indirect_table,
                               logical_index - VFS_INODE_DIRECT_BLOCKS);
    }
    if (*physical == 0U) {
        errno = EIO;
        return -1;
    }
    return validate_data_block(*physical);
}

static int load_indirect(vfs_disk_t *disk, const vfs_inode_t *inode,
                         uint8_t table[VFS_BLOCK_SIZE])
{
    memset(table, 0, VFS_BLOCK_SIZE);
    if (inode->indirect_block == 0U) return 0;
    if (validate_data_block(inode->indirect_block) == -1) return -1;
    return vfs_disk_read_block(disk, inode->indirect_block, table);
}

int vfs_file_create(vfs_disk_t *disk, uint32_t parent_ino,
                    const char *name, uint32_t *inode_number)
{
    if (disk == NULL || !disk->is_open || disk->fd < 0 ||
        name == NULL || inode_number == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (validate_disk(disk) == -1) return -1;

    uint32_t ino;
    if (vfs_alloc_inode(disk, &ino) == -1) return -1;

    uint64_t now = current_timestamp();
    vfs_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = (uint16_t)(VFS_MODE_REGULAR | 0644U);
    inode.links = 1U;
    inode.atime = inode.mtime = inode.ctime = now;
    if (vfs_inode_write(disk, ino, &inode) == -1) {
        int saved = errno;
        (void)vfs_free_inode(disk, ino);
        errno = saved;
        return -1;
    }
    if (vfs_dir_add(disk, parent_ino, name, ino,
                    VFS_DIRENT_REGULAR) == -1) {
        int saved = errno;
        memset(&inode, 0, sizeof(inode));
        if (vfs_inode_write(disk, ino, &inode) == 0)
            (void)vfs_free_inode(disk, ino);
        errno = saved;
        return -1;
    }
    *inode_number = ino;
    return 0;
}

int64_t vfs_file_read(vfs_disk_t *disk, uint32_t ino, void *buffer,
                      size_t count, uint64_t offset)
{
    if ((count > 0U && buffer == NULL) || count > (size_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    vfs_inode_t inode;
    if (read_regular_inode(disk, ino, &inode) == -1) return -1;
    if (count == 0U || offset >= inode.size) return 0;

    uint64_t available = inode.size - offset;
    size_t to_read = (uint64_t)count > available ? (size_t)available : count;
    uint8_t indirect[VFS_BLOCK_SIZE];
    if (load_indirect(disk, &inode, indirect) == -1) return -1;

    uint8_t *out = buffer;
    size_t done = 0U;
    while (done < to_read) {
        uint64_t pos = offset + done;
        uint32_t index = (uint32_t)(pos / VFS_BLOCK_SIZE);
        uint32_t in_block = (uint32_t)(pos % VFS_BLOCK_SIZE);
        uint32_t physical;
        if (get_data_block(&inode, indirect, index, &physical) == -1) return -1;
        uint8_t block[VFS_BLOCK_SIZE];
        if (vfs_disk_read_block(disk, physical, block) == -1) return -1;
        size_t chunk = VFS_BLOCK_SIZE - in_block;
        if (chunk > to_read - done) chunk = to_read - done;
        memcpy(out + done, block + in_block, chunk);
        done += chunk;
    }
    inode.atime = current_timestamp();
    if (vfs_inode_write(disk, ino, &inode) == -1) return -1;
    return (int64_t)done;
}

int64_t vfs_file_write(vfs_disk_t *disk, uint32_t ino, const void *buffer,
                       size_t count, uint64_t offset)
{
    if ((count > 0U && buffer == NULL) || count > (size_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    vfs_inode_t inode;
    if (read_regular_inode(disk, ino, &inode) == -1) return -1;
    if (count == 0U) return 0;
    if (offset > UINT64_MAX - (uint64_t)count) {
        errno = EFBIG;
        return -1;
    }
    uint64_t end = offset + (uint64_t)count;
    if (end > VFS_FILE_MAX_SIZE) {
        errno = EFBIG;
        return -1;
    }
    if (offset > inode.size) {
        errno = EINVAL;
        return -1;
    }

    uint32_t old_blocks = (uint32_t)((inode.size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE);
    uint32_t needed = (uint32_t)((end + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE);
    uint32_t newly_allocated[VFS_FILE_MAX_BLOCKS + 1U];
    uint32_t new_count = 0U;
    bool new_indirect = false;
    bool table_dirty = false;
    uint8_t indirect[VFS_BLOCK_SIZE];
    uint8_t old_indirect[VFS_BLOCK_SIZE];

    if (load_indirect(disk, &inode, indirect) == -1) return -1;
    memcpy(old_indirect, indirect, sizeof(indirect));

    if (needed > VFS_INODE_DIRECT_BLOCKS && inode.indirect_block == 0U) {
        uint32_t iblock;
        if (vfs_alloc_block(disk, &iblock) == -1) return -1;
        inode.indirect_block = iblock;
        memset(indirect, 0, sizeof(indirect));
        uint8_t zero[VFS_BLOCK_SIZE] = {0};
        if (vfs_disk_write_block(disk, iblock, zero) == -1) {
            int saved = errno;
            (void)vfs_free_block(disk, iblock);
            errno = saved;
            return -1;
        }
        newly_allocated[new_count++] = iblock;
        new_indirect = true;
        table_dirty = true;
    }

    for (uint32_t i = old_blocks; i < needed; i++) {
        uint32_t block;
        if (vfs_alloc_block(disk, &block) == -1) goto rollback;
        uint8_t zero[VFS_BLOCK_SIZE] = {0};
        if (vfs_disk_write_block(disk, block, zero) == -1) {
            int saved = errno;
            (void)vfs_free_block(disk, block);
            errno = saved;
            goto rollback;
        }
        newly_allocated[new_count++] = block;
        if (i < VFS_INODE_DIRECT_BLOCKS) {
            inode.direct_blocks[i] = block;
        } else {
            pointer_set(indirect, i - VFS_INODE_DIRECT_BLOCKS, block);
            table_dirty = true;
        }
    }

    {
        const uint8_t *input = buffer;
        size_t done = 0U;
        while (done < count) {
            uint64_t pos = offset + done;
            uint32_t index = (uint32_t)(pos / VFS_BLOCK_SIZE);
            uint32_t in_block = (uint32_t)(pos % VFS_BLOCK_SIZE);
            uint32_t physical;
            if (get_data_block(&inode, indirect, index, &physical) == -1) goto rollback;
            uint8_t block[VFS_BLOCK_SIZE];
            if (vfs_disk_read_block(disk, physical, block) == -1) goto rollback;
            size_t chunk = VFS_BLOCK_SIZE - in_block;
            if (chunk > count - done) chunk = count - done;
            memcpy(block + in_block, input + done, chunk);
            if (vfs_disk_write_block(disk, physical, block) == -1) goto rollback;
            done += chunk;
        }
    }

    if (table_dirty && vfs_disk_write_block(disk, inode.indirect_block, indirect) == -1)
        goto rollback;
    if (end > inode.size) inode.size = end;
    inode.mtime = inode.ctime = current_timestamp();
    if (vfs_inode_write(disk, ino, &inode) == -1) goto rollback;
    return (int64_t)count;

rollback:
    {
        int saved = errno;
        if (inode.indirect_block != 0U && !new_indirect && table_dirty)
            (void)vfs_disk_write_block(disk, inode.indirect_block, old_indirect);
        for (uint32_t i = 0U; i < new_count; i++)
            (void)vfs_free_block(disk, newly_allocated[i]);
        errno = saved;
        return -1;
    }
}

int vfs_file_truncate(vfs_disk_t *disk, uint32_t ino, uint64_t new_size)
{
    if (new_size > VFS_FILE_MAX_SIZE) {
        errno = EFBIG;
        return -1;
    }
    vfs_inode_t inode;
    if (read_regular_inode(disk, ino, &inode) == -1) return -1;
    if (new_size == inode.size) return 0;

    if (new_size > inode.size) {
        uint64_t extension = new_size - inode.size;
        if (extension > SIZE_MAX) {
            errno = EFBIG;
            return -1;
        }
        uint8_t *zeros = calloc((size_t)extension == 0U ? 1U : (size_t)extension, 1U);
        if (zeros == NULL) return -1;
        int64_t written = vfs_file_write(disk, ino, zeros, (size_t)extension, inode.size);
        int saved = errno;
        free(zeros);
        errno = saved;
        if (written < 0) return -1;
        if ((uint64_t)written != extension) {
            errno = EIO;
            return -1;
        }
        return 0;
    }

    uint8_t indirect[VFS_BLOCK_SIZE];
    if (load_indirect(disk, &inode, indirect) == -1) return -1;
    uint8_t old_indirect[VFS_BLOCK_SIZE];
    memcpy(old_indirect, indirect, sizeof(indirect));

    uint32_t required = (uint32_t)((new_size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE);
    if (new_size > 0U && new_size % VFS_BLOCK_SIZE != 0U) {
        uint32_t physical;
        uint32_t index = (uint32_t)(new_size / VFS_BLOCK_SIZE);
        if (get_data_block(&inode, indirect, index, &physical) == -1) return -1;
        uint8_t block[VFS_BLOCK_SIZE];
        if (vfs_disk_read_block(disk, physical, block) == -1) return -1;
        memset(block + (size_t)(new_size % VFS_BLOCK_SIZE), 0,
               VFS_BLOCK_SIZE - (size_t)(new_size % VFS_BLOCK_SIZE));
        if (vfs_disk_write_block(disk, physical, block) == -1) return -1;
    }

    uint32_t to_free[VFS_FILE_MAX_BLOCKS];
    uint32_t free_count = 0U;
    for (uint32_t i = required; i < VFS_FILE_MAX_BLOCKS; i++) {
        uint32_t physical = 0U;
        if (i < VFS_INODE_DIRECT_BLOCKS) {
            physical = inode.direct_blocks[i];
            inode.direct_blocks[i] = 0U;
        } else if (inode.indirect_block != 0U) {
            uint32_t pi = i - VFS_INODE_DIRECT_BLOCKS;
            physical = pointer_get(indirect, pi);
            if (physical != 0U) pointer_set(indirect, pi, 0U);
        }
        if (physical != 0U) {
            if (validate_data_block(physical) == -1) return -1;
            to_free[free_count++] = physical;
        }
    }

    bool keep_indirect = false;
    if (inode.indirect_block != 0U) {
        for (uint32_t i = 0U; i < PTRS_PER_INDIRECT; i++) {
            if (pointer_get(indirect, i) != 0U) {
                keep_indirect = true;
                break;
            }
        }
    }
    uint32_t indirect_to_free = 0U;
    if (inode.indirect_block != 0U && !keep_indirect) {
        indirect_to_free = inode.indirect_block;
        inode.indirect_block = 0U;
    }

    inode.size = new_size;
    inode.mtime = inode.ctime = current_timestamp();
    if (inode.indirect_block != 0U &&
        vfs_disk_write_block(disk, inode.indirect_block, indirect) == -1) {
        return -1;
    }
    if (vfs_inode_write(disk, ino, &inode) == -1) {
        if (inode.indirect_block != 0U)
            (void)vfs_disk_write_block(disk, inode.indirect_block, old_indirect);
        return -1;
    }

    for (uint32_t i = 0U; i < free_count; i++) {
        if (vfs_free_block(disk, to_free[i]) == -1) return -1;
    }
    if (indirect_to_free != 0U && vfs_free_block(disk, indirect_to_free) == -1) return -1;
    return 0;
}
