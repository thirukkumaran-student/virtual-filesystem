#ifndef VFS_DISK_H
#define VFS_DISK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Virtual disk geometry.
 *
 * 4096 blocks * 4096 bytes = 16 MiB.
 */
#define VFS_BLOCK_SIZE   4096U
#define VFS_TOTAL_BLOCKS 4096U
#define VFS_DISK_SIZE    ((uint64_t)VFS_BLOCK_SIZE * VFS_TOTAL_BLOCKS)

/*
 * Represents an open virtual disk image.
 *
 * fd       : POSIX file descriptor.
 * is_open  : Indicates whether the handle is initialized and open.
 */
typedef struct {
    int fd;
    bool is_open;
} vfs_disk_t;

/*
 * Creates a new disk image with the configured size.
 *
 * Returns:
 *   0  on success
 *  -1  on failure; errno indicates the error
 *
 * Existing files must never be overwritten.
 */
int vfs_disk_create(const char *path);

/*
 * Opens an existing disk image.
 *
 * The image must be a regular file of exactly VFS_DISK_SIZE bytes.
 * Opening an image does not format or modify its contents.
 */
int vfs_disk_open(vfs_disk_t *disk, const char *path);

/*
 * Reads one complete block into buffer.
 *
 * block_number must be less than VFS_TOTAL_BLOCKS.
 */
int vfs_disk_read_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    void *buffer
);

/*
 * Writes one complete block from buffer.
 *
 * block_number must be less than VFS_TOTAL_BLOCKS.
 */
int vfs_disk_write_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    const void *buffer
);

/*
 * Flushes pending file data and metadata to the underlying storage.
 */
int vfs_disk_sync(vfs_disk_t *disk);

/*
 * Closes an open disk handle.
 */
int vfs_disk_close(vfs_disk_t *disk);

#endif