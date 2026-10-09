
#define _POSIX_C_SOURCE 200809L

#include "vfs_cli.h"
#include "vfs_dir.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_alloc.h"
#include "vfs_file.h"
#include <time.h>
#include "vfs_check.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFS_PROMPT "VFS> "
#define VFS_INPUT_SIZE 4096U
#define VFS_MAX_DIRECTORY_DEPTH 128U
#define DIRENT_HEADER_SIZE 8U
#define DIRENT_INODE_OFFSET 0U
#define DIRENT_RECLEN_OFFSET 4U
#define DIRENT_TYPE_OFFSET 6U
#define DIRENT_NAMELEN_OFFSET 7U
#define DIRENT_NAME_OFFSET 8U

typedef struct {
    uint32_t current_inode;
    size_t depth;
    char components[VFS_MAX_DIRECTORY_DEPTH][VFS_NAME_MAX + 1U];
} directory_context_t;

static uint16_t read_u16_le(const uint8_t *buffer)
{
    return (uint16_t)(
        (uint16_t)buffer[0] |
        ((uint16_t)buffer[1] << 8U)
    );
}

static uint32_t read_u32_le(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8U) |
        ((uint32_t)buffer[2] << 16U) |
        ((uint32_t)buffer[3] << 24U);
}

static void write_u16_le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void write_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void print_help(void)
{
    printf("\nAvailable commands\n");
    printf("==================\n\n");

    printf("Directory commands:\n");
    printf("  makedir <name>       Create a child directory\n");
    printf("  list                 List directory entries\n");
    printf("  changedir <name>     Enter a child directory\n");
    printf("  removedir <name>     Remove an empty child directory\n");
    printf("  back                 Move to the parent directory\n");
    printf("  where                Display the current path\n\n");

    printf("Filesystem commands:\n");
    printf("  create <name>        Create an empty regular file\n");
    printf("  read <name>          Display file contents\n");
    printf("  write <name> <text>  Replace file contents\n");
    printf("  append <name> <text> Add text to the end of a file\n");
    printf("  stat <name>          Display file metadata\n");
    printf("  delete <name>        Delete a regular file\n");
    printf("  check                Check filesystem consistency\n");
    printf("  info                 Display filesystem information\n\n");

    printf("General commands:\n");
    printf("  help                 Display this help message\n");
    printf("  exit                 Close the virtual filesystem\n\n");
}

static void print_filesystem_info(vfs_disk_t *disk)
{
    uint8_t superblock[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk,
            VFS_SUPERBLOCK_BLOCK,
            superblock) == -1) {
        perror("Error reading filesystem information");
        return;
    }

    uint32_t version = read_u32_le(superblock + 8U);
    uint32_t block_size = read_u32_le(superblock + 12U);
    uint32_t total_blocks = read_u32_le(superblock + 16U);
    uint32_t inode_count = read_u32_le(superblock + 20U);
    uint32_t inode_size = read_u32_le(superblock + 24U);
    uint32_t free_blocks = read_u32_le(superblock + 64U);
    uint32_t free_inodes = read_u32_le(superblock + 68U);

    uint64_t total_capacity =
        (uint64_t)block_size * total_blocks;

    uint64_t free_capacity =
        (uint64_t)block_size * free_blocks;

    printf("\nFilesystem Information\n");
    printf("======================\n");
    printf("Filesystem:       Virtual Filesystem\n");
    printf("Version:          %" PRIu32 "\n", version);
    printf("Block size:       %" PRIu32 " bytes\n", block_size);
    printf("Total blocks:     %" PRIu32 "\n", total_blocks);
    printf("Total capacity:   %" PRIu64 " bytes (%.2f MiB)\n",
           total_capacity,
           (double)total_capacity / (1024.0 * 1024.0));
    printf("Inode count:      %" PRIu32 "\n", inode_count);
    printf("Inode size:       %" PRIu32 " bytes\n", inode_size);
    printf("Free blocks:      %" PRIu32 "\n", free_blocks);
    printf("Free capacity:    %" PRIu64 " bytes (%.2f MiB)\n",
           free_capacity,
           (double)free_capacity / (1024.0 * 1024.0));
    printf("Free inodes:      %" PRIu32 "\n\n", free_inodes);
}

/*
 * Print directory entries directly from the existing on-disk format.
 * Each record is validated before its fields are accessed.
 */
static int print_directory_contents(
    vfs_disk_t *disk,
    uint32_t directory_inode_number)
{
    vfs_inode_t directory;

    if (vfs_inode_read(
            disk,
            directory_inode_number,
            &directory) == -1) {
        return -1;
    }

    if ((directory.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    if (directory.size == 0U ||
        directory.size >
            (uint64_t)VFS_INODE_DIRECT_BLOCKS * VFS_BLOCK_SIZE) {
        errno = EIO;
        return -1;
    }

    printf("\n%-6s %-8s %s\n", "TYPE", "INODE", "NAME");
    printf("--------------------------------\n");

    uint64_t remaining = directory.size;

    for (uint32_t i = 0U;
         i < VFS_INODE_DIRECT_BLOCKS && remaining > 0U;
         i++) {
        uint32_t block_number = directory.direct_blocks[i];

        if (block_number < VFS_DATA_START ||
            block_number >= VFS_TOTAL_BLOCKS) {
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
            if (limit - offset < DIRENT_HEADER_SIZE) {
                errno = EIO;
                return -1;
            }

            const uint8_t *entry = block + offset;

            uint32_t inode_number =
                read_u32_le(entry + DIRENT_INODE_OFFSET);

            uint32_t record_length =
                read_u16_le(entry + DIRENT_RECLEN_OFFSET);

            uint8_t type = entry[DIRENT_TYPE_OFFSET];
            uint32_t name_length = entry[DIRENT_NAMELEN_OFFSET];

            if (record_length < DIRENT_HEADER_SIZE ||
                record_length % 4U != 0U ||
                record_length > limit - offset ||
                name_length > VFS_NAME_MAX ||
                name_length > record_length - DIRENT_HEADER_SIZE) {
                errno = EIO;
                return -1;
            }

            /*
             * A zero name length denotes an unused directory record.
             * Inode zero is valid and must not be treated as deletion.
             */
            if (name_length != 0U) {
                if (inode_number >= VFS_INODE_COUNT ||
                    (type != VFS_DIRENT_REGULAR &&
                     type != VFS_DIRENT_DIRECTORY)) {
                    errno = EIO;
                    return -1;
                }

                char name[VFS_NAME_MAX + 1U];

                memcpy(
                    name,
                    entry + DIRENT_NAME_OFFSET,
                    name_length
                );
                name[name_length] = '\0';

                const char *type_name =
                    type == VFS_DIRENT_DIRECTORY ? "DIR" : "FILE";

                printf("%-6s %-8" PRIu32 " %s\n",
                       type_name,
                       inode_number,
                       name);
            }

            offset += record_length;
        }

        remaining -= limit;
    }

    if (remaining != 0U) {
        errno = EIO;
        return -1;
    }

    printf("\n");
    return 0;
}

static void cleanup_new_directory(
    vfs_disk_t *disk,
    uint32_t inode_number,
    uint32_t block_number)
{
    int saved_errno = errno;

    (void)vfs_free_inode(disk, inode_number);
    (void)vfs_free_block(disk, block_number);

    errno = saved_errno;
}

static int create_directory(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name)
{
    if (name == NULL ||
        name[0] == '\0' ||
        strlen(name) > VFS_NAME_MAX ||
        strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t existing_inode;

    if (vfs_dir_lookup(
            disk,
            parent_inode_number,
            name,
            &existing_inode) == 0) {
        errno = EEXIST;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    vfs_inode_t parent;

    if (vfs_inode_read(
            disk,
            parent_inode_number,
            &parent) == -1) {
        return -1;
    }

    if ((parent.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    if (parent.links == UINT16_MAX) {
        errno = EMLINK;
        return -1;
    }

    uint32_t new_inode_number;

    if (vfs_alloc_inode(disk, &new_inode_number) == -1) {
        return -1;
    }

    uint32_t new_block_number;

    if (vfs_alloc_block(disk, &new_block_number) == -1) {
        int saved_errno = errno;
        (void)vfs_free_inode(disk, new_inode_number);
        errno = saved_errno;
        return -1;
    }

    /*
     * Initialize the directory data block.
     *
     * "."  -> the new directory
     * ".." -> the parent directory
     */
    uint8_t block[VFS_BLOCK_SIZE] = {0};

    write_u32_le(block, new_inode_number);
    write_u16_le(block + 4U, 12U);
    block[6U] = VFS_DIRENT_DIRECTORY;
    block[7U] = 1U;
    block[8U] = '.';

    write_u32_le(block + 12U, parent_inode_number);
    write_u16_le(
        block + 16U,
        (uint16_t)(VFS_BLOCK_SIZE - 12U));
    block[18U] = VFS_DIRENT_DIRECTORY;
    block[19U] = 2U;
    block[20U] = '.';
    block[21U] = '.';

    if (vfs_disk_write_block(
            disk,
            new_block_number,
            block) == -1) {
        int saved_errno = errno;
        cleanup_new_directory(
            disk, new_inode_number, new_block_number);
        errno = saved_errno;
        return -1;
    }

    /*
     * Initialize and persist the new directory inode.
     */
    vfs_inode_t child = {0};

    child.mode = (uint16_t)(
        VFS_MODE_DIRECTORY | VFS_MODE_DEFAULT);
    child.links = 2U;
    child.size = VFS_BLOCK_SIZE;
    child.direct_blocks[0] = new_block_number;

    time_t current_time = time(NULL);
    uint64_t timestamp = current_time < 0
        ? 0U
        : (uint64_t)current_time;

    child.atime = timestamp;
    child.mtime = timestamp;
    child.ctime = timestamp;

    if (vfs_inode_write(
            disk,
            new_inode_number,
            &child) == -1) {
        int saved_errno = errno;
        cleanup_new_directory(
            disk, new_inode_number, new_block_number);
        errno = saved_errno;
        return -1;
    }

    /*
     * Add the new directory to its parent.
     */
    if (vfs_dir_add(
            disk,
            parent_inode_number,
            name,
            new_inode_number,
            VFS_DIRENT_DIRECTORY) == -1) {
        int saved_errno = errno;
        cleanup_new_directory(
            disk, new_inode_number, new_block_number);
        errno = saved_errno;
        return -1;
    }

    /*
     * vfs_dir_add() may have changed the parent inode's size
     * or block pointers. Read it again to avoid overwriting
     * those changes with a stale copy.
     */
    if (vfs_inode_read(
            disk,
            parent_inode_number,
            &parent) == -1) {
        int saved_errno = errno;

        (void)vfs_dir_remove(
            disk, parent_inode_number, name);

        cleanup_new_directory(
            disk, new_inode_number, new_block_number);

        errno = saved_errno;
        return -1;
    }

    if (parent.links == UINT16_MAX) {
        int saved_errno = EMLINK;

        (void)vfs_dir_remove(
            disk, parent_inode_number, name);

        cleanup_new_directory(
            disk, new_inode_number, new_block_number);

        errno = saved_errno;
        return -1;
    }

    parent.links++;

    if (vfs_inode_write(
            disk,
            parent_inode_number,
            &parent) == -1) {
        int saved_errno = errno;

        (void)vfs_dir_remove(
            disk, parent_inode_number, name);

        cleanup_new_directory(
            disk, new_inode_number, new_block_number);

        errno = saved_errno;
        return -1;
    }

    printf(
        "Directory '%s' created successfully (inode %" PRIu32 ").\n",
        name,
        new_inode_number);

    return 0;
}

static int remove_empty_directory(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name)
{
    if (name == NULL ||
        name[0] == '\0' ||
        strlen(name) > VFS_NAME_MAX ||
        strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t child_inode_number;

    if (vfs_dir_lookup(
            disk,
            parent_inode_number,
            name,
            &child_inode_number) == -1) {
        return -1;
    }

    vfs_inode_t child;

    if (vfs_inode_read(
            disk,
            child_inode_number,
            &child) == -1) {
        return -1;
    }

    if ((child.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    /*
     * Current makedir creates one full data block per directory.
     * Reject any directory layout this implementation cannot safely
     * reclaim.
     */
    if (child.size != VFS_BLOCK_SIZE ||
        child.direct_blocks[0] <= VFS_DATA_START ||
        child.direct_blocks[0] >= VFS_TOTAL_BLOCKS ||
        child.indirect_block != 0U ||
        child.links != 2U) {
        errno = EUCLEAN;
        return -1;
    }

    for (uint32_t i = 1U; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        if (child.direct_blocks[i] != 0U) {
            errno = EUCLEAN;
            return -1;
        }
    }

    uint32_t child_block_number = child.direct_blocks[0];

    uint8_t block[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk,
            child_block_number,
            block) == -1) {
        return -1;
    }

    bool found_dot = false;
    bool found_dotdot = false;
    uint32_t offset = 0U;

    while (offset < VFS_BLOCK_SIZE) {
        if (VFS_BLOCK_SIZE - offset < DIRENT_HEADER_SIZE) {
            errno = EUCLEAN;
            return -1;
        }

        const uint8_t *entry = block + offset;

        uint32_t entry_inode =
            read_u32_le(entry + DIRENT_INODE_OFFSET);
        uint32_t record_length =
            read_u16_le(entry + DIRENT_RECLEN_OFFSET);
        uint8_t type = entry[DIRENT_TYPE_OFFSET];
        uint32_t name_length = entry[DIRENT_NAMELEN_OFFSET];

        if (record_length < DIRENT_HEADER_SIZE ||
            record_length % 4U != 0U ||
            record_length > VFS_BLOCK_SIZE - offset ||
            name_length > record_length - DIRENT_HEADER_SIZE ||
            name_length > VFS_NAME_MAX) {
            errno = EUCLEAN;
            return -1;
        }


        if (name_length != 0U) {
            const char *entry_name =
                (const char *)(entry + DIRENT_NAME_OFFSET);

            if (name_length == 1U &&
                entry_name[0] == '.') {
                if (type != VFS_DIRENT_DIRECTORY ||
                    found_dot ||
                    entry_inode != child_inode_number) {
                    errno = EUCLEAN;
                    return -1;
                }

                found_dot = true;
            } else if (name_length == 2U &&
                       entry_name[0] == '.' &&
                       entry_name[1] == '.') {
                if (type != VFS_DIRENT_DIRECTORY ||
                    found_dotdot ||
                    entry_inode != parent_inode_number) {
                    errno = EUCLEAN;
                    return -1;
                }

                found_dotdot = true;
            } else {
                if (type != VFS_DIRENT_REGULAR &&
                    type != VFS_DIRENT_DIRECTORY) {
                    errno = EUCLEAN;
                    return -1;
                }

                errno = ENOTEMPTY;
                return -1;
            }
        }


        offset += record_length;
    }

    if (!found_dot || !found_dotdot) {
        errno = EUCLEAN;
        return -1;
    }

    vfs_inode_t parent;

    if (vfs_inode_read(
            disk,
            parent_inode_number,
            &parent) == -1) {
        return -1;
    }

    if ((parent.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    if (parent.links <= 2U) {
        errno = EUCLEAN;
        return -1;
    }

    /*
     * Remove the directory entry before releasing its resources.
     */
    if (vfs_dir_remove(
            disk,
            parent_inode_number,
            name) == -1) {
        return -1;
    }

    if (vfs_free_block(disk, child_block_number) == -1) {
        return -1;
    }

    if (vfs_free_inode(disk, child_inode_number) == -1) {
        return -1;
    }

    parent.links--;

    if (vfs_inode_write(
            disk,
            parent_inode_number,
            &parent) == -1) {
        return -1;
    }

    printf(
        "Directory '%s' removed successfully (inode %" PRIu32 ").\n",
        name,
        child_inode_number
    );

    return 0;
}

static void print_current_path(const directory_context_t *context)
{
    printf("/");

    for (size_t i = 0U; i < context->depth; i++) {
        printf("%s", context->components[i]);

        if (i + 1U < context->depth) {
            printf("/");
        }
    }

    printf("\n");
}

/*
 * Move to the parent using the directory's actual ".." entry.
 */
static int change_to_parent(
    vfs_disk_t *disk,
    directory_context_t *context)
{
    if (context->depth == 0U) {
        printf("Already at the root directory.\n");
        return 0;
    }

    uint32_t parent_inode;

    if (vfs_dir_lookup(
            disk,
            context->current_inode,
            "..",
            &parent_inode) == -1) {
        return -1;
    }

    vfs_inode_t parent;

    if (vfs_inode_read(disk, parent_inode, &parent) == -1) {
        return -1;
    }

    if ((parent.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        errno = EIO;
        return -1;
    }

    context->current_inode = parent_inode;
    context->depth--;

    return 0;
}

static int change_directory(
    vfs_disk_t *disk,
    directory_context_t *context,
    const char *name)
{
    if (name == NULL || name[0] == '\0') {
        printf("Usage: changedir <name>\n");
        return 0;
    }

    if (strcmp(name, ".") == 0) {
        return 0;
    }

    if (strcmp(name, "..") == 0) {
        if (change_to_parent(disk, context) == -1) {
            perror("Error changing directory");
        }
        return 0;
    }

    if (strlen(name) > VFS_NAME_MAX || strchr(name, '/') != NULL) {
        printf("Error: enter a valid child directory name.\n");
        return 0;
    }

    if (context->depth >= VFS_MAX_DIRECTORY_DEPTH) {
        printf("Error: maximum directory depth reached.\n");
        return 0;
    }

    uint32_t child_inode;

    if (vfs_dir_lookup(
            disk,
            context->current_inode,
            name,
            &child_inode) == -1) {
        if (errno == ENOENT) {
            printf("Error: entry '%s' was not found.\n", name);
        } else {
            perror("Error looking up directory");
        }
        return 0;
    }

    vfs_inode_t child;

    if (vfs_inode_read(disk, child_inode, &child) == -1) {
        perror("Error reading directory inode");
        return 0;
    }

    if ((child.mode & 0170000U) != VFS_MODE_DIRECTORY) {
        printf("Error: '%s' is not a directory.\n", name);
        return 0;
    }

    context->current_inode = child_inode;

    memcpy(
        context->components[context->depth],
        name,
        strlen(name) + 1U
    );

    context->depth++;

    return 0;
}

static int resolve_regular_file(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name,
    uint32_t *file_inode_number)
{
    if (name == NULL ||
        name[0] == '\0' ||
        strlen(name) > VFS_NAME_MAX ||
        strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    if (vfs_dir_lookup(
            disk,
            parent_inode_number,
            name,
            file_inode_number) == -1) {
        return -1;
    }

    vfs_inode_t file;

    if (vfs_inode_read(
            disk,
            *file_inode_number,
            &file) == -1) {
        return -1;
    }

    if ((file.mode & 0170000U) != VFS_MODE_REGULAR) {
        errno = EISDIR;
        return -1;
    }

    return 0;
}

static int write_file_contents(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *arguments)
{
    char parsed[VFS_INPUT_SIZE];

    size_t argument_length = strlen(arguments);

    if (argument_length >= sizeof(parsed)) {
        errno = E2BIG;
        return -1;
    }

    memcpy(parsed, arguments, argument_length + 1U);

    char *text = strpbrk(parsed, " \t");

    if (text == NULL) {
        printf("Usage: write <name> <text>\n");
        return 0;
    }

    *text++ = '\0';

    while (*text == ' ' || *text == '\t') {
        text++;
    }

    if (*text == '\0') {
        printf("Usage: write <name> <text>\n");
        return 0;
    }

    uint32_t inode_number;

    if (resolve_regular_file(
            disk,
            parent_inode_number,
            parsed,
            &inode_number) == -1) {
        return -1;
    }

    size_t text_length = strlen(text);

    int64_t written = vfs_file_write(
        disk,
        inode_number,
        text,
        text_length,
        0U);

    if (written == -1) {
        return -1;
    }

    if ((uint64_t)written != (uint64_t)text_length) {
        errno = EIO;
        return -1;
    }

    /*
     * Writing at offset zero does not necessarily remove old
     * trailing bytes. Truncate to the new content length.
     */
    if (vfs_file_truncate(
            disk,
            inode_number,
            text_length) == -1) {
        return -1;
    }

    printf(
        "Wrote %zu bytes to '%s'.\n",
        text_length,
        parsed);

    return 0;
}

static int append_file_contents(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *arguments)
{
    char parsed[VFS_INPUT_SIZE];

    size_t argument_length = strlen(arguments);

    if (argument_length >= sizeof(parsed)) {
        errno = E2BIG;
        return -1;
    }

    memcpy(parsed, arguments, argument_length + 1U);

    char *text = strpbrk(parsed, " \t");

    if (text == NULL) {
        printf("Usage: append <name> <text>\n");
        return 0;
    }

    *text++ = '\0';

    while (*text == ' ' || *text == '\t') {
        text++;
    }

    if (*text == '\0') {
        printf("Usage: append <name> <text>\n");
        return 0;
    }

    uint32_t inode_number;

    if (resolve_regular_file(
            disk,
            parent_inode_number,
            parsed,
            &inode_number) == -1) {
        return -1;
    }

    vfs_inode_t file;

    if (vfs_inode_read(disk, inode_number, &file) == -1) {
        return -1;
    }

    const uint64_t maximum_size =
        (uint64_t)VFS_INODE_DIRECT_BLOCKS * VFS_BLOCK_SIZE;

    size_t text_length = strlen(text);

    if (file.size > maximum_size ||
        (uint64_t)text_length > maximum_size - file.size) {
        errno = EFBIG;
        return -1;
    }

    int64_t written = vfs_file_write(
        disk,
        inode_number,
        text,
        text_length,
        file.size);

    if (written == -1) {
        return -1;
    }

    if ((uint64_t)written != (uint64_t)text_length) {
        errno = EIO;
        return -1;
    }

    printf(
        "Appended %zu bytes to '%s'.\n",
        text_length,
        parsed);

    return 0;
}

static int read_file_contents(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name)
{
    uint32_t inode_number;

    if (resolve_regular_file(
            disk,
            parent_inode_number,
            name,
            &inode_number) == -1) {
        return -1;
    }

    vfs_inode_t file;

    if (vfs_inode_read(disk, inode_number, &file) == -1) {
        return -1;
    }

    if (file.size > VFS_INODE_DIRECT_BLOCKS * (uint64_t)VFS_BLOCK_SIZE) {
        errno = EFBIG;
        return -1;
    }

    size_t file_size = (size_t)file.size;

    uint8_t *buffer = malloc(file_size == 0U ? 1U : file_size);

    if (buffer == NULL) {
        return -1;
    }

    int64_t bytes_read = vfs_file_read(
        disk,
        inode_number,
        buffer,
        file_size,
        0U);

    if (bytes_read == -1) {
        int saved_errno = errno;
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    if ((uint64_t)bytes_read != file.size) {
        free(buffer);
        errno = EIO;
        return -1;
    }

    printf("\n--- %s ---\n", name);

    if (file_size > 0U &&
        fwrite(buffer, 1U, file_size, stdout) != file_size) {
        int saved_errno = errno;
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    if (file_size == 0U || buffer[file_size - 1U] != '\n') {
        putchar('\n');
    }

    printf("--- End of file ---\n\n");

    free(buffer);
    return 0;
}

static int print_file_stat(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name)
{
    uint32_t inode_number;

    if (resolve_regular_file(
            disk,
            parent_inode_number,
            name,
            &inode_number) == -1) {
        return -1;
    }

    vfs_inode_t file;

    if (vfs_inode_read(disk, inode_number, &file) == -1) {
        return -1;
    }

    printf("\nFile Metadata\n");
    printf("=============\n");
    printf("Name:       %s\n", name);
    printf("Inode:      %" PRIu32 "\n", inode_number);
    printf("Type:       Regular file\n");
    printf("Mode:       %04o\n", (unsigned)(file.mode & 07777U));
    printf("UID:        %u\n", (unsigned)file.uid);
    printf("GID:        %u\n", (unsigned)file.gid);
    printf("Links:      %u\n", (unsigned)file.links);
    printf("Size:       %" PRIu64 " bytes\n", file.size);
    printf("Accessed:   %" PRIu64 " (Unix timestamp)\n", file.atime);
    printf("Modified:   %" PRIu64 " (Unix timestamp)\n", file.mtime);
    printf("Changed:    %" PRIu64 " (Unix timestamp)\n\n", file.ctime);

    return 0;
}

static int delete_regular_file(
    vfs_disk_t *disk,
    uint32_t parent_inode_number,
    const char *name)
{
    uint32_t inode_number;

    if (resolve_regular_file(
            disk,
            parent_inode_number,
            name,
            &inode_number) == -1) {
        return -1;
    }

    vfs_inode_t file;

    if (vfs_inode_read(
            disk,
            inode_number,
            &file) == -1) {
        return -1;
    }

    /*
     * Validate all direct block pointers before releasing any blocks.
     * This implementation supports direct blocks only.
     */

    if (file.size > VFS_FILE_MAX_SIZE) {
        errno = EUCLEAN;
        return -1;
    }

    uint32_t required_blocks = (uint32_t)(
        (file.size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE
    );

    for (uint32_t i = 0; i < VFS_INODE_DIRECT_BLOCKS; i++) {
        uint32_t block_number = file.direct_blocks[i];

        if (i < required_blocks && block_number == 0U) {
            errno = EUCLEAN;
            return -1;
        }

        if (block_number == 0U) {
            continue;
        }

        if (block_number < VFS_DATA_START ||
            block_number >= VFS_TOTAL_BLOCKS) {
            errno = EUCLEAN;
            return -1;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (file.direct_blocks[j] == block_number) {
                errno = EUCLEAN;
                return -1;
            }
        }
    }
    /*
     * Remove the name first. If that fails, leave the file intact.
     */
    if (vfs_dir_remove(
            disk,
            parent_inode_number,
            name) == -1) {
        return -1;
    }

    /*
     * Truncation releases the file's data blocks.
     */
    if (vfs_file_truncate(disk, inode_number, 0U) == -1) {
        return -1;
    }

    /*
     * Finally, release the inode allocation bit.
     */
    if (vfs_free_inode(disk, inode_number) == -1) {
        return -1;
    }

    printf(
        "File '%s' deleted successfully (inode %" PRIu32 ").\n",
        name,
        inode_number
    );

    return 0;
}

static void trim_input(char *input)
{
    size_t length = strlen(input);

    while (length > 0U &&
           (input[length - 1U] == '\n' ||
            input[length - 1U] == '\r' ||
            input[length - 1U] == ' ' ||
            input[length - 1U] == '\t')) {
        input[--length] = '\0';
    }

    size_t start = 0U;

    while (input[start] == ' ' || input[start] == '\t') {
        start++;
    }

    if (start > 0U) {
        memmove(input, input + start, strlen(input + start) + 1U);
    }
}

static int process_command(
    vfs_disk_t *disk,
    directory_context_t *context,
    const char *input)
{
    char command[VFS_INPUT_SIZE];

    size_t length = strcspn(input, " \t");

    if (length >= sizeof(command)) {
        printf("Error: command is too long.\n");
        return 0;
    }

    memcpy(command, input, length);
    command[length] = '\0';

    const char *arguments = input + length;

    while (*arguments == ' ' || *arguments == '\t') {
        arguments++;
    }

    if (strcmp(command, "help") == 0) {
        if (*arguments != '\0') {
            printf("Usage: help\n");
            return 0;
        }

        print_help();
        return 0;
    }

    if (strcmp(command, "info") == 0) {
        if (*arguments != '\0') {
            printf("Usage: info\n");
            return 0;
        }

        print_filesystem_info(disk);
        return 0;
    }

    if (strcmp(command, "makedir") == 0) {
    if (*arguments == '\0') {
        printf("Usage: makedir <name>\n");
        return 0;
    }

    if (create_directory(
            disk,
            context->current_inode,
            arguments) == -1) {
        perror("Error creating directory");
    }

    return 0;
}

    if (strcmp(command, "removedir") == 0) {
        if (*arguments == '\0' ||
            strpbrk(arguments, " \t") != NULL) {
            printf("Usage: removedir <name>\n");
            return 0;
        }

        if (remove_empty_directory(
                disk,
                context->current_inode,
                arguments) == -1) {
            perror("Error removing directory");
        }

        return 0;
    }

    if (strcmp(command, "list") == 0) {
        if (*arguments != '\0') {
            printf("Usage: list\n");
            return 0;
        }

        if (print_directory_contents(
                disk,
                context->current_inode) == -1) {
            perror("Error listing directory");
        }

        return 0;
    }

    if (strcmp(command, "create") == 0) {
    if (*arguments == '\0') {
        printf("Usage: create <name>\n");
        return 0;
    }

    uint32_t inode_number;

    if (vfs_file_create(
            disk,
            context->current_inode,
            arguments,
            &inode_number) == -1) {
        perror("Error creating file");
    } else {
        printf(
            "File '%s' created successfully (inode %" PRIu32 ").\n",
            arguments,
            inode_number);
    }

    return 0;
}

if (strcmp(command, "write") == 0) {
    if (*arguments == '\0') {
        printf("Usage: write <name> <text>\n");
        return 0;
    }

    if (write_file_contents(
            disk,
            context->current_inode,
            arguments) == -1) {
        perror("Error writing file");
    }

    return 0;
}

if (strcmp(command, "read") == 0) {
    if (*arguments == '\0' ||
        strpbrk(arguments, " \t") != NULL) {
        printf("Usage: read <name>\n");
        return 0;
    }

    if (read_file_contents(
            disk,
            context->current_inode,
            arguments) == -1) {
        perror("Error reading file");
    }

    return 0;
}

if (strcmp(command, "append") == 0) {
    if (*arguments == '\0') {
        printf("Usage: append <name> <text>\n");
        return 0;
    }

    if (append_file_contents(
            disk,
            context->current_inode,
            arguments) == -1) {
        perror("Error appending to file");
    }

    return 0;
}

if (strcmp(command, "stat") == 0) {
    if (*arguments == '\0' ||
        strpbrk(arguments, " \t") != NULL) {
        printf("Usage: stat <name>\n");
        return 0;
    }

    if (print_file_stat(
            disk,
            context->current_inode,
            arguments) == -1) {
        perror("Error reading file metadata");
    }

    return 0;
}

    if (strcmp(command, "delete") == 0) {
        if (*arguments == '\0' ||
            strpbrk(arguments, " \t") != NULL) {
            printf("Usage: delete <name>\n");
            return 0;
        }

        if (delete_regular_file(
                disk,
                context->current_inode,
                arguments) == -1) {
            perror("Error deleting file");
        }

        return 0;
    }


    if (strcmp(command, "check") == 0) {
        if (*arguments != '\0') {
            printf("Usage: check\n");
            return 0;
        }

        if (vfs_check(disk, stdout) == -1) {
            perror("Error checking filesystem");
        }

        return 0;
    }

    if (strcmp(command, "changedir") == 0) {
        if (*arguments == '\0') {
            printf("Usage: changedir <name>\n");
            return 0;
        }

        (void)change_directory(disk, context, arguments);
        return 0;
    }

    if (strcmp(command, "back") == 0) {
        if (*arguments != '\0') {
            printf("Usage: back\n");
            return 0;
        }

        if (change_to_parent(disk, context) == -1) {
            perror("Error moving to parent directory");
        }

        return 0;
    }

    if (strcmp(command, "where") == 0) {
        if (*arguments != '\0') {
            printf("Usage: where\n");
            return 0;
        }

        print_current_path(context);
        return 0;
    }

    if (strcmp(command, "exit") == 0) {
        if (*arguments != '\0') {
            printf("Usage: exit\n");
            return 0;
        }

        printf("Goodbye.\n");
        return 1;
    }

    printf("Unknown command: %s\n", command);
    printf("Type 'help' to see available commands.\n");

    return 0;
}

int vfs_cli_run(vfs_disk_t *disk)
{
    char input[VFS_INPUT_SIZE];

    if (disk == NULL || !disk->is_open) {
        fprintf(stderr, "Error: filesystem disk is not open.\n");
        return 1;
    }

    directory_context_t context = {
        .current_inode = VFS_ROOT_INODE,
        .depth = 0U
    };

    printf("\nVirtual Filesystem\n");
    printf("Type 'help' to see available commands.\n\n");

    for (;;) {
        printf(VFS_PROMPT);
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (ferror(stdin)) {
                perror("Error reading command");
                return 1;
            }

            printf("\n");
            return 0;
        }

        size_t length = strlen(input);

        if (length > 0U &&
            input[length - 1U] != '\n' &&
            !feof(stdin)) {
            int character;

            while ((character = getchar()) != '\n' &&
                   character != EOF) {
                /* Discard the rest of the oversized input line. */
            }

            printf("Error: command is too long.\n");
            continue;
        }

        trim_input(input);

        if (input[0] == '\0') {
            continue;
        }

        if (process_command(disk, &context, input)) {
            return 0;
        }
    }
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s format <disk-image>\n", program);
    fprintf(stderr, "  %s <disk-image>\n", program);
}

int main(int argc, char *argv[])
{
    vfs_disk_t disk = {
        .fd = -1,
        .is_open = false
    };

    if (argc == 3 && strcmp(argv[1], "format") == 0) {
        if (vfs_disk_create(argv[2]) == -1) {
            perror("Error creating disk image");
            return EXIT_FAILURE;
        }

        if (vfs_disk_open(&disk, argv[2]) == -1) {
            perror("Error opening disk image");
            return EXIT_FAILURE;
        }

        if (vfs_format(&disk) == -1) {
            int saved_errno = errno;
            (void)vfs_disk_close(&disk);
            errno = saved_errno;
            perror("Error formatting filesystem");
            return EXIT_FAILURE;
        }

        if (vfs_disk_close(&disk) == -1) {
            perror("Error closing disk image");
            return EXIT_FAILURE;
        }

        printf("Filesystem formatted successfully: %s\n", argv[2]);
        return EXIT_SUCCESS;
    }

    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (vfs_disk_open(&disk, argv[1]) == -1) {
        perror("Error opening disk image");
        return EXIT_FAILURE;
    }

    int formatted = vfs_is_formatted(&disk);

    if (formatted != 1) {
        if (formatted == 0) {
            fprintf(stderr,
                    "Error: disk image does not contain a recognized "
                    "filesystem.\n");
        } else {
            perror("Error validating filesystem");
        }

        (void)vfs_disk_close(&disk);
        return EXIT_FAILURE;
    }

    printf("Opened filesystem: %s\n", argv[1]);

    int result = vfs_cli_run(&disk);

    if (vfs_disk_sync(&disk) == -1) {
        perror("Error synchronizing filesystem");
        result = 1;
    }

    if (vfs_disk_close(&disk) == -1) {
        perror("Error closing filesystem");
        result = 1;
    }

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
