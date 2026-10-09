#ifndef VFS_FORMAT_H
#define VFS_FORMAT_H

#include "vfs_disk.h"

#include <stdint.h>

/* Filesystem format identification. */
#define VFS_MAGIC "VFSFS001"
#define VFS_MAGIC_SIZE 8U
#define VFS_FORMAT_VERSION 1U

/* Inode configuration. */
#define VFS_INODE_COUNT 1024U
#define VFS_INODE_SIZE 128U
#define VFS_ROOT_INODE 0U

/* Filesystem metadata locations. */
#define VFS_SUPERBLOCK_BLOCK 0U
#define VFS_BLOCK_BITMAP_BLOCK 1U
#define VFS_INODE_BITMAP_BLOCK 2U
#define VFS_INODE_TABLE_START 3U
#define VFS_INODE_TABLE_BLOCKS 32U
#define VFS_JOURNAL_START 35U
#define VFS_JOURNAL_BLOCKS 64U
#define VFS_DATA_START 99U
#define VFS_DATA_BLOCKS (VFS_TOTAL_BLOCKS - VFS_DATA_START)

/* Inode type and permission bits. */
#define VFS_MODE_DIRECTORY 0040000U
#define VFS_MODE_REGULAR   0100000U
#define VFS_MODE_DEFAULT   0755U

/* Directory entry types. */
#define VFS_DIRENT_DIRECTORY 2U

/*
 * Format an open virtual disk.
 *
 * WARNING: This operation initializes the filesystem and destroys
 * any existing contents of the disk image.
 *
 * Returns:
 *   0  on success
 *  -1  on failure; errno indicates the error
 */
int vfs_format(vfs_disk_t *disk);

/*
 * Check whether an open disk contains a recognized filesystem.
 *
 * Returns:
 *   1  if the superblock has the expected format and geometry
 *   0  if the disk is not formatted or has an invalid superblock
 *  -1  if a disk I/O error occurs
 */
int vfs_is_formatted(vfs_disk_t *disk);

#endif