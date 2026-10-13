# Design and Implementation of a Linux-Based Virtual File System and Storage Management Engine

## 1. Overview

This project implements a Linux-inspired virtual filesystem in C11. It manages files and directories inside a disk-image-backed storage system using a custom filesystem format, inode metadata, allocation bitmaps, directory records, and block-based storage.

The disk image is stored as an ordinary file on the host Linux filesystem. The application interprets its contents as a separate virtual storage device and exposes filesystem operations through an interactive command-line interface (CLI).

**This is a user-space filesystem implementation, not a Linux kernel filesystem driver.** It does not mount the image as a conventional operating-system filesystem.

## 2. Objectives

- Design a deterministic, explicitly serialized on-disk filesystem format.
- Implement virtual disk creation, block I/O, synchronization, and persistence.
- Manage filesystem metadata through a superblock, inodes, and allocation bitmaps.
- Allocate and reclaim data blocks and inode records.
- Implement file creation, reading, writing, appending, truncation, and deletion.
- Implement directory creation, lookup, navigation, and removal.
- Support direct and single-indirect file block addressing.
- Improve repeated block access through an LRU block cache.
- Accelerate directory lookup through an in-memory hash index.
- Track storage operations and cache/index performance metrics.
- Detect selected filesystem consistency errors.
- Validate the implementation using automated unit tests and CLI integration tests.

## 3. Technology Stack

| Component | Technology |
|---|---|
| Implementation language | C11 |
| Operating environment | Linux, Ubuntu, or WSL2 |
| Storage backend | Disk-image file |
| Low-level I/O | POSIX file operations |
| Compiler | GCC |
| Build system | GNU Make |
| Testing | C unit tests and shell integration tests |
| Version control | Git and GitHub |

## 4. Architecture

The implementation is organized into modular components.

| Module | Responsibility |
|---|---|
| `vfs_disk` | Disk-image creation, opening, block I/O, caching, metrics, synchronization, and closing |
| `vfs_format` | Filesystem formatting, initialization, and superblock validation |
| `vfs_alloc` | Block and inode bitmap allocation and release |
| `vfs_inode` | Inode serialization, deserialization, and metadata access |
| `vfs_dir` | Directory entry management, lookup, insertion, removal, and in-memory indexing |
| `vfs_file` | File creation, reading, writing, appending, truncation, and deletion support |
| `vfs_check` | Filesystem metadata, directory-record, and block-allocation consistency validation |
| `main` | Interactive command-line interface and command dispatch |

### Architecture workflow

1. **CLI layer:** Receives commands and invokes the corresponding filesystem operation.
2. **Filesystem layer:** Performs file and directory operations using inode and allocation interfaces.
3. **Metadata layer:** Manages inodes, directory records, bitmaps, and filesystem counters.
4. **Virtual disk layer:** Reads and writes fixed-size blocks in the disk image, using an in-memory cache.
5. **Consistency layer:** Examines filesystem metadata and referenced blocks to detect selected structural errors.

Persistent metadata is serialized explicitly in little-endian byte order rather than written directly from native C structures. This makes the on-disk representation independent of native structure padding.

## 5. Virtual Disk Format

The filesystem uses a fixed geometry.

| Parameter | Value |
|---|---:|
| Block size | 4,096 bytes |
| Total blocks | 4,096 |
| Virtual disk capacity | 16 MiB |
| Inode count | 1,024 |
| Inode record size | 128 bytes |
| Reserved journal area | 64 blocks |
| Root inode number | 0 |
| Block cache capacity | 16 blocks |

### 5.1 On-disk layout

| Block numbers | Purpose |
|---|---|
| 0 | Superblock |
| 1 | Block allocation bitmap |
| 2 | Inode allocation bitmap |
| 3–34 | Inode table |
| 35–98 | Reserved journal area |
| 99 | Initial root directory data block |
| 100–4095 | General data-block allocation region |

The root directory initially uses block 99. General data-block allocations begin at block 100.

The journal area is reserved in the disk layout, but journal transaction processing and crash recovery are not implemented.

### 5.2 Metadata representation

- **Superblock:** Stores filesystem geometry, metadata boundaries, counters, and format information.
- **Block bitmap:** Tracks allocated and free blocks.
- **Inode bitmap:** Tracks allocated and free inodes.
- **Inode:** Stores file or directory metadata, size, type, and block pointers.
- **Directory records:** Store names, inode references, and entry types.
- **Data blocks:** Store file contents and directory records.

See [`docs/filesystem-format.md`](docs/filesystem-format.md) for the detailed on-disk format specification.

### 5.3 File block addressing

Regular files use:

- **Eight direct block pointers:** Point directly to file data blocks.
- **One single-indirect pointer:** Points to a block containing up to 1,024 additional data-block pointers.

With 4,096-byte blocks and 32-bit block pointers, the theoretical maximum file size is:

\[
(8 + 1024)\times 4096
= 4,227,072\text{ bytes}
\]

This is approximately 4.03 MiB per file, subject to successful block allocation and the implementation's operational constraints.

## 6. Implemented Features

### 6.1 Virtual disk and filesystem initialization

- Create a new disk image.
- Open an existing disk image.
- Format the image with filesystem metadata.
- Reject attempts to format an existing image.
- Validate filesystem geometry and superblock information.
- Synchronize disk-image changes.

### 6.2 File management

- Create regular files.
- Read file contents.
- Replace existing file contents.
- Append data to a file.
- Truncate or extend files.
- Maintain file size and inode metadata.
- Support direct and single-indirect block addressing.
- Reclaim data blocks when files are truncated or deleted.
- Preserve file contents across clean exit and reopen operations.

### 6.3 Directory management

- Create directories.
- List directory entries.
- Navigate into a directory and return to its parent.
- Display the current directory path.
- Look up directory entries by name.
- Insert and remove directory records.
- Reject duplicate names and invalid operations.
- Remove empty directories.
- Maintain an in-memory hash index for directory lookup.

The directory index is an optimization, not persistent filesystem metadata. It is rebuilt from directory records when needed after reopening the disk image and is invalidated when directory entries change.

### 6.4 Block cache and performance metrics

The virtual disk layer implements a **16-block LRU (Least Recently Used) cache**.

- Repeated reads can be served from cached blocks.
- Cache entries are updated when writes occur.
- Least-recently-used entries are evicted when the cache is full.
- Cache state is in memory and does not persist across application sessions.

The implementation tracks logical reads and writes, physical block reads and writes, cache hits, cache misses, and directory-index activity.

### 6.5 Filesystem consistency checker

The `check` command validates selected filesystem invariants, including:

- Superblock geometry and metadata boundaries.
- Allocation of reserved blocks and the root inode.
- Free-block and free-inode counters.
- Inode types and file-size constraints.
- Direct and single-indirect block pointers.
- Block allocation status and duplicate block ownership.
- Allocated blocks without recognized references.
- Directory-record structure and filename validity.
- Directory entries referencing invalid or unallocated inodes.
- Directory-entry types that disagree with target inode types.

The checker is diagnostic. It reports detected inconsistencies; it does not repair corrupted metadata automatically.

## 7. Build and Test

### 7.1 Prerequisites

On Ubuntu or WSL2, install GCC and GNU Make:

```bash
sudo apt update
sudo apt install build-essential
```

### 7.2 Compile

From the project root:

```bash
make
```

The executable is generated at:

```text
build/vfs
```

The build uses C11 and strict compiler warnings, including `-Wall`, `-Wextra`, `-Wpedantic`, and `-Werror`.

### 7.3 Run the automated tests

```bash
make test
```

The test target compiles and executes the unit-test binaries and CLI integration script.

The current test suite passes:

- **186 unit-test checks**
- **15 CLI integration checks**
- **201 checks in total**

Coverage includes allocation, inode management, directory operations and indexing, disk caching, file operations, indirect blocks, filesystem formatting, corruption detection, persistence, and CLI workflows.

To rebuild from a clean state:

```bash
make clean
make
make test
```

To remove generated build artifacts:

```bash
make clean
```

## 8. Usage

Run the following commands from the project root.

### 8.1 Format a new virtual disk

```bash
./build/vfs format disk.img
```

This creates a new disk image and initializes the filesystem. The formatter refuses to overwrite an existing image.

### 8.2 Open the filesystem

```bash
./build/vfs disk.img
```

The application opens the image and starts the interactive CLI.

### 8.3 Available CLI commands

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
| `metrics` | Display disk-cache and directory-index metrics |
| `exit` | Exit the CLI |

### 8.4 Example session

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
vfs> metrics
vfs> exit
```

The exact output formatting may differ from this example.

### 8.5 Verify persistence

After exiting the application, reopen the same image:

```bash
./build/vfs disk.img
```

Then run:

```text
vfs> list
vfs> changedir documents
vfs> read notes.txt
```

The directory entry and file contents should persist across clean application exits and subsequent reopen operations.

### 8.6 Inspect cache and directory-index metrics

Use:

```text
vfs> metrics
```

The output reports logical and physical I/O counts, cache hits and misses, cache hit ratio, directory lookup counts, directory-index reuse, and index builds.

These measurements help demonstrate how repeated accesses can reduce physical disk-image reads and how the in-memory directory index avoids repeatedly scanning directory records.

## 9. Testing Strategy

The project uses C unit tests and a shell-based CLI integration test.

| Test module | Main coverage |
|---|---|
| `test_disk.c` | Disk-image creation, block I/O, caching, eviction, and error handling |
| `test_format.c` | Filesystem formatting, geometry, bitmaps, root initialization, and invalid superblocks |
| `test_alloc.c` | Block and inode allocation, release, counters, and invalid operations |
| `test_inode.c` | Inode metadata, allocation validation, serialization, and persistence |
| `test_dir.c` | Directory operations, hash collisions, index invalidation, reuse, and rebuilding |
| `test_file.c` | File reads and writes, truncation, indirect addressing, and persistence |
| `test_check.c` | Clean filesystem validation and detection of selected metadata corruption |
| `test_cli.sh` | End-to-end commands, directory lifecycle, file operations, consistency checks, and reopening |

The corruption tests verify that the checker detects selected invalid directory references and malformed records. They also verify that the checker does not modify the corrupted directory block during inspection.

The test suite provides evidence for the implemented behaviors; it does not establish complete crash consistency, recovery correctness, or filesystem correctness under concurrent access.

## 10. Current Limitations

The following capabilities are not implemented or are not comprehensively validated:

- Linux kernel integration or mounting through the operating system's VFS.
- Journal transaction processing and crash recovery.
- Transactional rollback for operations that update multiple metadata structures.
- Comprehensive Unix permission enforcement.
- Concurrent access coordination and locking.
- Automatic repair of filesystem corruption.
- Comprehensive fault-injection and crash-consistency testing.
- Complete validation of every filesystem invariant, including all possible link-count and directory-structure conditions.

The reserved 64-block journal region is part of the disk layout. **It does not mean journaling or recovery is implemented.**

The in-memory block cache and directory index improve repeated access, but they are not persistent structures. Their state is reconstructed or reset as appropriate when the image is reopened.

## 11. Project Status

The current version provides a functioning user-space virtual filesystem with:

- A custom disk-image format.
- Inode-based metadata and bitmap allocation.
- Persistent file and directory operations.
- Direct and single-indirect file block addressing.
- An LRU block cache and storage-operation metrics.
- An in-memory directory hash index and lookup metrics.
- A filesystem consistency checker with directory-integrity validation.
- Automated unit and CLI integration tests.

The project is designed as an educational storage-engine implementation. It demonstrates filesystem organization, metadata management, block allocation, directory indexing, caching, persistence, and integrity validation without implementing a kernel-level filesystem driver.

## 12. Repository Structure

```text
virtual-fs/
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
│   ├── test_check.c
│   ├── test_cli.sh
│   ├── test_dir.c
│   ├── test_disk.c
│   ├── test_file.c
│   ├── test_format.c
│   └── test_inode.c
├── docs/
│   └── filesystem-format.md
├── Makefile
├── README.md
└── .gitignore
```

The `build/` directory contains generated executables and test binaries after compilation. Disk images created during tests and demonstrations are runtime artifacts rather than source files.

## 13. Future Work

Potential extensions include:

1. Define a journal record format and implement journal transaction processing and recovery.
2. Improve crash consistency and atomicity across multi-step filesystem operations.
3. Expand consistency checking to cover additional inode, directory, and link-count invariants.
4. Implement permission checks with a documented credential model.
5. Add concurrency control and locking.
6. Expand fault-injection tests and performance benchmarks.
7. Evaluate cache and directory-index behavior with larger workloads.

These extensions should be implemented incrementally, retaining strict compiler warnings and automated tests throughout development.

## 14. License

Refer to the repository's license declaration and project ownership terms for permissions governing reuse, modification, and redistribution.