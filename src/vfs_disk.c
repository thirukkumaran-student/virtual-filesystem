#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Ensures that a disk handle is open and usable.
 */
static int validate_disk(const vfs_disk_t *disk)
{
    if (disk == NULL || !disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

/*
 * Creates a new virtual disk image.
 *
 * O_EXCL prevents accidental overwriting of an existing file.
 * ftruncate establishes the exact image size.
 */
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

/*
 * Opens an existing disk image without formatting it.
 */
int vfs_disk_open(vfs_disk_t *disk, const char *path)
{
    int fd;
    struct stat file_info;

    if (disk == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /*
     * Initialize the output handle before attempting to open the file.
     */
    disk->fd = -1;
    disk->is_open = false;

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
 * Reads exactly one block.
 *
 * pread() allows block access without changing the file descriptor's
 * shared file offset.
 *
 * The loop handles interrupted calls and partial reads.
 */
int vfs_disk_read_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    void *buffer
)
{
    size_t completed = 0;
    off_t offset;

    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (buffer == NULL || block_number >= VFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    offset = (off_t)((uint64_t)block_number * VFS_BLOCK_SIZE);

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

        /*
         * A zero-byte read before the requested block is complete
         * indicates an unexpected end of file.
         */
        if (bytes_read == 0) {
            errno = EIO;
            return -1;
        }

        completed += (size_t)bytes_read;
    }

    return 0;
}

/*
 * Writes exactly one block.
 *
 * The loop handles interrupted calls and partial writes.
 */
int vfs_disk_write_block(
    vfs_disk_t *disk,
    uint32_t block_number,
    const void *buffer
)
{
    size_t completed = 0;
    off_t offset;

    if (validate_disk(disk) == -1) {
        return -1;
    }

    if (buffer == NULL || block_number >= VFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    offset = (off_t)((uint64_t)block_number * VFS_BLOCK_SIZE);

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

            return -1;
        }

        if (bytes_written == 0) {
            errno = EIO;
            return -1;
        }

        completed += (size_t)bytes_written;
    }

    return 0;
}

/*
 * Forces pending changes to the disk image through fsync().
 */
int vfs_disk_sync(vfs_disk_t *disk)
{
    if (validate_disk(disk) == -1) {
        return -1;
    }

    return fsync(disk->fd);
}

/*
 * Closes an open disk image.
 *
 * The handle is invalidated before close() so callers cannot
 * accidentally reuse it after this function returns.
 */
int vfs_disk_close(vfs_disk_t *disk)
{
    int fd;

    if (disk == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!disk->is_open || disk->fd < 0) {
        errno = EBADF;
        return -1;
    }

    fd = disk->fd;

    disk->fd = -1;
    disk->is_open = false;

    return close(fd);
}