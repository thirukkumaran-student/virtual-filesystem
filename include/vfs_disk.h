
#ifndef VFS_DISK_H
#define VFS_DISK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Virtual disk geometry.
 * 4096 blocks * 4096 bytes = 16 MiB.
 */
#define VFS_BLOCK_SIZE   4096U
#define VFS_TOTAL_BLOCKS 4096U
#define VFS_DISK_SIZE    ((uint64_t)VFS_BLOCK_SIZE * VFS_TOTAL_BLOCKS)

/* Number of blocks held in the in-memory LRU cache. */
#define VFS_CACHE_CAPACITY 16U

typedef struct {
    uint64_t logical_reads;
    uint64_t logical_writes;
    uint64_t physical_reads;
    uint64_t physical_writes;
    uint64_t cache_hits;
    uint64_t cache_misses;
} vfs_disk_stats_t;

typedef struct {
    bool valid;
    uint32_t block_number;
    uint64_t last_used;
    uint8_t data[VFS_BLOCK_SIZE];
} vfs_cache_entry_t;

/*
 * Open virtual disk and its in-memory cache.
 *
 * Cache entries are never part of the on-disk format.
 * Writes use write-through semantics.
 */
typedef struct {
    int fd;
    bool is_open;

    vfs_cache_entry_t cache[VFS_CACHE_CAPACITY];
    uint64_t cache_clock;
    vfs_disk_stats_t stats;
} vfs_disk_t;

int vfs_disk_create(const char *path);

int vfs_disk_open(vfs_disk_t *disk, const char *path);

int vfs_disk_read_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    void *buffer
);

int vfs_disk_write_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    const void *buffer
);

int vfs_disk_sync(vfs_disk_t *disk);

int vfs_disk_close(vfs_disk_t *disk);

#endif
