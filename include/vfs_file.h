#ifndef VFS_FILE_H
#define VFS_FILE_H

#include "vfs_disk.h"
#include "vfs_inode.h"
#include <stddef.h>
#include <stdint.h>

#define VFS_INDIRECT_POINTERS (VFS_BLOCK_SIZE / sizeof(uint32_t))
#define VFS_FILE_MAX_BLOCKS (VFS_INODE_DIRECT_BLOCKS + VFS_INDIRECT_POINTERS)
#define VFS_FILE_MAX_SIZE ((uint64_t)VFS_FILE_MAX_BLOCKS * VFS_BLOCK_SIZE)

int vfs_file_create(vfs_disk_t *disk, uint32_t parent_inode_number,
                    const char *name, uint32_t *inode_number);
int64_t vfs_file_read(vfs_disk_t *disk, uint32_t inode_number,
                      void *buffer, size_t count, uint64_t offset);
int64_t vfs_file_write(vfs_disk_t *disk, uint32_t inode_number,
                       const void *buffer, size_t count, uint64_t offset);
int vfs_file_truncate(vfs_disk_t *disk, uint32_t inode_number,
                      uint64_t new_size);

#endif
