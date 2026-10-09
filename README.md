# Design and Implementation of a Linux-Based Virtual File System and Storage Management Engine

## 1. Overview

This project implements a Linux-inspired virtual filesystem in C11. It manages files and directories inside a virtual disk image, using filesystem metadata, inode records, allocation bitmaps, directory entries, and block-based storage.

The disk image is stored as an ordinary file on the host Linux filesystem. The project interprets its contents as a separate virtual storage device.

**This is a user-space filesystem implementation, not a Linux kernel filesystem driver.** It does not mount the image as a normal operating-system filesystem.

## 2. Objectives

- Design a deterministic, versioned on-disk filesystem format.
- Implement disk-image creation, opening, reading, writing, and persistence.
- Manage filesystem metadata through serialized superblock and inode records.
- Allocate and release blocks and inodes using bitmaps.
- Support file and directory operations.
- Preserve filesystem contents across clean program exits and subsequent reopen operations.
- Detect selected filesystem consistency errors.
- Validate functionality using unit tests and CLI integration tests.

## 3. Technology Stack

| Component | Technology |
|---|---|
| Implementation language | C11 |
| Operating environment | Linux / Ubuntu / WSL2 |
| Storage backend | Disk-image file |
| Low-level I/O | POSIX file operations |
| Build system | GNU Make |
| Compiler | GCC |
| Testing | C unit tests and shell integration tests |
| Version control | Git |

## 4. Architecture

The implementation is organized into separate modules.

| Module | Responsibility |
|---|---|
| `vfs_disk` | Disk-image creation, opening, block I/O, synchronization, and closing |
| `vfs_format` | Filesystem formatting and superblock validation |
| `vfs_alloc` | Block and inode bitmap allocation and release |
| `vfs_inode` | Inode serialization, deserialization, and metadata access |
| `vfs_dir` | Directory entry lookup, insertion, and removal |
| `vfs_file` | File creation, reading, writing, truncation, and deletion support |
| `vfs_check` | Filesystem consistency validation |
| `main` | Command-line interface and operation dispatch |

Persistent metadata is serialized explicitly in little-endian byte order rather than being written directly from native C structures.

## 5. Virtual Disk Format

The current filesystem geometry is:

| Parameter | Value |
|---|---:|
| Block size | 4,096 bytes |
| Total blocks | 4,096 |
| Virtual disk capacity | 16 MiB |
| Inode count | 1,024 |
| Inode record size | 128 bytes |
| Reserved journal area | 64 blocks |
| Root inode number | 0 |

### On-disk layout

| Blocks | Purpose |
|---|---|
| 0 | Superblock |
| 1 | Block bitmap |
| 2 | Inode bitmap |
| 3–34 | Inode table |
| 35–98 | Reserved journal area |
| 99–4095 | Data region |

The root directory initially uses block 99. Subsequent allocations use available data blocks.

### File and directory representation

- The superblock stores filesystem geometry, counters, region boundaries, and format metadata.
- Bitmaps track allocated blocks and inodes.
- Inodes store object metadata and block pointers.
- Directories store variable-length directory records.
- Regular files currently use eight direct block pointers.
- The current regular-file size limit is 32 KiB.

See [`docs/filesystem-format.md`](docs/filesystem-format.md) for the detailed on-disk format specification.

## 6. Implemented Features

- Create a new virtual disk image and format it.
- Open an existing formatted image.
- Create, list, and navigate directories.
- Create regular files.
- Read, overwrite, and append file contents.
- Display file metadata.
- Delete regular files.
- Remove empty directories.
- Allocate and release blocks and inodes.
- Preserve data across clean exit and reopen.
- Check selected filesystem metadata and block-allocation invariants.
- Run unit and CLI integration tests.

## 7. Build and Test

### Prerequisites

Install GCC and GNU Make on Ubuntu:

```bash
sudo apt update
sudo apt install build-essential
```

### Compile

From the project root:

```bash
make
```

The executable is generated at:

```text
build/vfs
```

### Run the tests

```bash
make test
```

The test target runs the unit-test binaries and the CLI integration test script.

The current test suite passes **139 unit-test assertions and 15 CLI integration checks**, for a total of 154 checks.

To remove generated build artifacts:

```bash
make clean
```

## 8. Usage

Run all commands from the project root.

### Format a new virtual disk

```bash
./build/vfs format disk.img
```

Formatting creates a new disk image. The formatter does not overwrite an existing image.

### Open the filesystem

```bash
./build/vfs disk.img
```

Once the CLI starts, use the commands below.

| Command | Purpose |
|---|---|
| `help` | Display available commands |
| `info` | Display filesystem information |
| `list` | List entries in the current directory |
| `changedir <name>` | Enter a directory |
| `back` | Return to the parent directory |
| `where` | Display the current directory path |
| `makedir <name>` | Create a directory |
| `removedir <name>` | Remove an empty directory |
| `create <name>` | Create a regular file |
| `read <name>` | Display file contents |
| `write <name> <text>` | Replace file contents |
| `append <name> <text>` | Append text to a file |
| `stat <name>` | Display file metadata |
| `delete <name>` | Delete a regular file |
| `check` | Run the filesystem consistency checker |
| `exit` | Exit the CLI |

For example:

```text
vfs> makedir documents
vfs> changedir documents
vfs> create notes.txt
vfs> write notes.txt Hello filesystem
vfs> append notes.txt !
vfs> read notes.txt
Hello filesystem!
vfs> stat notes.txt
vfs> check
vfs> exit
```

The exact output formatting may differ from this example.

### Verify persistence

After exiting, reopen the same image:

```bash
./build/vfs disk.img
```

Then use `list` and `read notes.txt` to verify that the directory entry and file contents persist.

## 9. Consistency Checking

The `check` command validates selected filesystem invariants, including:

- Superblock geometry and metadata boundaries.
- Allocation of reserved blocks.
- Root inode allocation and expected root data block.
- Free-block and free-inode counters.
- Inode types, sizes, and direct block pointers.
- Allocation status and duplicate ownership of referenced blocks.
- Detection of allocated blocks that are not referenced by recognized filesystem objects.

The checker is a diagnostic tool, not a complete filesystem repair utility. It does not yet validate every directory reference, link count, or recovery scenario.

## 10. Current Limitations

The following capabilities are not implemented in the current version:

- Linux kernel integration or mounting through the operating system's VFS.
- Single-indirect block traversal, despite the field reserved in the inode format.
- Files larger than 32 KiB.
- Journal transaction processing and crash recovery.
- Transactional rollback for operations that update multiple metadata structures.
- Comprehensive Unix permission enforcement.
- A block cache and cache-performance metrics.
- Concurrent access coordination and locking.
- Automatic repair of filesystem corruption.
- Comprehensive fault-injection and crash-consistency testing.

The 64-block journal region is reserved on disk; **reservation does not mean journaling or recovery is implemented**.

## 11. Project Status

The current version provides a functioning user-space virtual filesystem with persistent file and directory operations, allocation management, a command-line interface, a consistency checker, and automated tests.

The format specification describes additional design goals. Those goals should be considered planned work until their implementation and tests are complete.

## 12. Repository Structure

```text
virtual-filesystem/
├── include/
│   ├── vfs_alloc.h
│   ├── vfs_check.h
│   ├── vfs_cli.h
│   ├── vfs_dir.h
│   ├── vfs_disk.h
│   ├── vfs_file.h
│   ├── vfs_format.h
│   └── vfs_inode.h
├── src/
│   ├── main.c
│   ├── vfs_alloc.c
│   ├── vfs_check.c
│   ├── vfs_dir.c
│   ├── vfs_disk.c
│   ├── vfs_file.c
│   ├── vfs_format.c
│   └── vfs_inode.c
├── tests/
│   ├── test_alloc.c
│   ├── test_cli.sh
│   ├── test_dir.c
│   ├── test_disk.c
│   ├── test_file.c
│   ├── test_format.c
│   └── test_inode.c
├── docs/
│   └── filesystem-format.md
├── Makefile
└── README.md
```

## 13. Future Work

Potential extensions include:

1. Implementing and testing single-indirect block addressing.
2. Strengthening directory and inode-reference validation.
3. Defining a journal record format and implementing recovery.
4. Adding permission checks and a documented credential model.
5. Implementing caching and storage-operation metrics.
6. Adding fault-injection tests and performance benchmarks.

These extensions should be implemented incrementally, with strict compiler warnings and automated tests retained throughout development.
