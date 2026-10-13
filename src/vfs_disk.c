
#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int validate_disk(const vfs_disk_t *disk)
{
    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

/*
 * Find a cached block.
 */
static int cache_find(const vfs_disk_t *disk, uint32_t block_number)
{
    for (uint32_t i = 0; i < VFS_CACHE_CAPACITY; i++) {
        if (disk->cache[i].valid &&
            disk->cache[i].block_number == block_number) {
            return (int)i;
        }
    }

    return -1;
}

/*
 * Select an invalid cache slot, or evict the least recently
 * used entry when all slots are occupied.
 */
static uint32_t cache_select_slot(const vfs_disk_t *disk)
{
    uint32_t oldest_slot = 0U;
    uint64_t oldest_time = UINT64_MAX;

    for (uint32_t i = 0; i < VFS_CACHE_CAPACITY; i++) {
        if (!disk->cache[i].valid) {
            return i;
        }

        if (disk->cache[i].last_used < oldest_time) {
            oldest_time = disk->cache[i].last_used;
            oldest_slot = i;
        }
    }

    return oldest_slot;
}

/*
 * Update recency information.
 */
static void cache_touch(vfs_disk_t *disk, uint32_t slot)
{
    /*
     * Avoid a zero timestamp after unsigned counter wraparound.
     * Cache eviction remains safe even if recency ordering resets.
     */
    if (disk->cache_clock == UINT64_MAX) {
        disk->cache_clock = 0U;

        for (uint32_t i = 0; i < VFS_CACHE_CAPACITY; i++) {
            disk->cache[i].last_used = 0U;
        }
    }

    disk->cache_clock++;
    disk->cache[slot].last_used = disk->cache_clock;
}

/*
 * Insert or update a block in the cache.
 */
static void cache_store(
    vfs_disk_t *disk,
    uint32_t block_number,
    const void *buffer
)
{
    int existing = cache_find(disk, block_number);
    uint32_t slot = existing >= 0
        ? (uint32_t)existing
        : cache_select_slot(disk);

    disk->cache[slot].valid = true;
    disk->cache[slot].block_number = block_number;
    memcpy(disk->cache[slot].data, buffer, VFS_BLOCK_SIZE);
    cache_touch(disk, slot);
}

int vfs_disk_create(const char *path)
{
    int fd;
    int saved_errno;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (fd == -1) {
        return -1;
    }

    if (ftruncate(fd, (off_t)VFS_DISK_SIZE) == -1) {
        saved_errno = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }

    if (fsync(fd) == -1) {
        saved_errno = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }

    if (close(fd) == -1) {
        saved_errno = errno;
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

int vfs_disk_open(vfs_disk_t *disk, const char *path)
{
    int fd;
    struct stat file_info;

    if (disk == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    disk->fd = -1;
    disk->is_open = false;
    memset(disk->cache, 0, sizeof(disk->cache));
    memset(&disk->stats, 0, sizeof(disk->stats));
    disk->cache_clock = 0U;

    fd = open(path, O_RDWR);

    if (fd == -1) {
        return -1;
    }

    if (fstat(fd, &file_info) == -1) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    if (!S_ISREG(file_info.st_mode)) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    if (file_info.st_size != (off_t)VFS_DISK_SIZE) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    disk->fd = fd;
    disk->is_open = true;

    return 0;
}

/*
 * Read-through cache:
 * - Cache hit: return the cached copy.
 * - Cache miss: read the disk, then populate the cache.
 */
int vfs_disk_read_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    void *buffer
)
{
    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (buffer == NULL || block_number >= VFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    disk->stats.logical_reads++;

    int slot = cache_find(disk, block_number);

    if (slot >= 0) {
        disk->stats.cache_hits++;
        cache_touch(disk, (uint32_t)slot);

        memcpy(
            buffer,
            disk->cache[slot].data,
            VFS_BLOCK_SIZE
        );

        return 0;
    }

    disk->stats.cache_misses++;

    size_t completed = 0U;
    off_t offset =
        (off_t)((uint64_t)block_number * VFS_BLOCK_SIZE);

    while (completed < VFS_BLOCK_SIZE) {
        ssize_t bytes_read = pread(
            disk->fd,
            (char *)buffer + completed,
            VFS_BLOCK_SIZE - completed,
            offset + (off_t)completed
        );

        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (bytes_read == 0) {
            errno = EIO;
            return -1;
        }

        completed += (size_t)bytes_read;
    }

    disk->stats.physical_reads++;
    cache_store(disk, block_number, buffer);

    return 0;
}

/*
 * Write-through cache:
 * - Write to the disk first.
 * - Update the cached copy only after the write succeeds.
 * - Invalidate a matching cache entry if the write fails.
 */
int vfs_disk_write_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    const void *buffer
)
{
    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (buffer == NULL || block_number >= VFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    disk->stats.logical_writes++;

    size_t completed = 0U;
    off_t offset =
        (off_t)((uint64_t)block_number * VFS_BLOCK_SIZE);

    while (completed < VFS_BLOCK_SIZE) {
        ssize_t bytes_written = pwrite(
            disk->fd,
            (const char *)buffer + completed,
            VFS_BLOCK_SIZE - completed,
            offset + (off_t)completed
        );

        if (bytes_written == -1) {
            if (errno == EINTR) {
                continue;
            }

            int slot = cache_find(disk, block_number);
            if (slot >= 0) {
                disk->cache[slot].valid = false;
            }

            return -1;
        }

        if (bytes_written == 0) {
            int slot = cache_find(disk, block_number);
            if (slot >= 0) {
                disk->cache[slot].valid = false;
            }

            errno = EIO;
            return -1;
        }

        completed += (size_t)bytes_written;
    }

    disk->stats.physical_writes++;
    cache_store(disk, block_number, buffer);

    return 0;
}

int vfs_disk_sync(vfs_disk_t *disk)
{
    if (validate_disk(disk) == -1) {
        return -1;
    }

    /*
     * The cache is write-through, so there are no dirty cache
     * entries to flush. fsync still flushes the underlying image.
     */
    return fsync(disk->fd);
}

int vfs_disk_close(vfs_disk_t *disk)
{
    if (disk == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    int fd = disk->fd;

    disk->fd = -1;
    disk->is_open = false;
    memset(disk->cache, 0, sizeof(disk->cache));

    return close(fd);
}
