# Virtual Filesystem: On-Disk Format Specification

**Project:** Linux-Based Virtual File System and Storage Management Engine  
**Format version:** 1  
**Status:** Initial design specification  
**Implementation language:** C11  
**Target environment:** Linux on WSL2

---

## 1. Design Goals

The filesystem is a user-space filesystem engine implemented in C. It
operates on a disk-image file and implements its own persistent namespace,
metadata, block allocation, directory management, caching, and recovery.

The host Linux filesystem stores the disk image as an ordinary file.
Our implementation interprets the bytes inside that file as a virtual disk.

The design prioritizes:

- Deterministic on-disk layout.
- Explicit serialization of persistent metadata.
- Persistent file and directory contents.
- Inode-based metadata.
- Bitmap-based allocation.
- Bounded file and directory operations.
- Detectable filesystem corruption.
- Testable transaction and recovery behavior.
- Versioned format evolution.

This project does not implement a kernel VFS module and does not replace
ext4 or another production filesystem.

## 2. Virtual Disk Geometry

Initial geometry:

| Parameter | Value |
|---|---:|
| Block size | 4096 bytes |
| Total blocks | 4096 |
| Virtual disk size | 16,777,216 bytes (16 MiB) |
| Valid block numbers | 0 through 4095 |
| Inode count | 1024 |
| Inode record size | 128 bytes |
| Journal size | 64 blocks (256 KiB) |
| Root inode number | 0 |

All block numbers are zero-based.

The byte offset of block `b` is:

    offset(b) = b * 4096

A block number is valid only when:

    0 <= b < 4096

All disk I/O must validate block numbers and byte ranges before accessing
the backing image.

## 3. On-Disk Region Layout

The initial layout is:

| Region | Start block | Block count | End block |
|---|---:|---:|---:|
| Superblock | 0 | 1 | 0 |
| Block bitmap | 1 | 1 | 1 |
| Inode bitmap | 2 | 1 | 2 |
| Inode table | 3 | 32 | 34 |
| Journal | 35 | 64 | 98 |
| Data region | 99 | 3997 | 4095 |

Region boundaries are inclusive at the end.

The inode table occupies:

    1024 inodes * 128 bytes = 131072 bytes = 32 blocks

The block bitmap requires 4096 bits, or 512 bytes. It occupies one
4096-byte block.

The inode bitmap requires 1024 bits, or 128 bytes. It occupies one
4096-byte block.

The data region initially contains 3997 blocks. Some of these blocks
will be allocated to directories, regular-file contents, and indirect
block pointer arrays.

The block bitmap describes the entire virtual disk, not only the data
region. Blocks reserved for filesystem metadata must be marked allocated.

## 4. Persistent Encoding Rules

Persistent structures must not depend on the compiler's native C
structure layout.

The implementation must:

- Use fixed-width integer types for persistent fields.
- Define explicit byte offsets and field widths.
- Serialize and deserialize fields explicitly.
- Use a documented byte order: little-endian.
- Validate magic values, format versions, region boundaries, and sizes
  before trusting metadata.
- Avoid writing pointers, `size_t`, native `time_t`, or compiler-dependent
  enum representations to disk.
- Use compile-time assertions for in-memory structures where appropriate.

In-memory C structures may be used internally, but persistent data must
pass through serialization/deserialization functions.

Reserved fields must be written as zero and validated according to the
format version's rules.

## 5. Superblock

Block 0 contains the superblock. The serialized superblock must fit
within one block.

The superblock records at least:

- Magic signature.
- Format version.
- Superblock structure length.
- Block size.
- Total block count.
- Inode count.
- Inode record size.
- Free-block count.
- Free-inode count.
- Block bitmap start and length.
- Inode bitmap start and length.
- Inode table start and length.
- Journal start and length.
- Data-region start and length.
- Root inode number.
- Filesystem state.
- Format timestamp.
- Last successful mount timestamp.
- Last metadata update timestamp.
- Reserved bytes for future compatible extensions.

The initial format uses block size 4096, total block count 4096,
inode count 1024, inode record size 128, root inode 0, and the region
boundaries specified in Section 3.

The exact serialized field offsets and magic signature must be defined
in the implementation header and kept consistent with this document.

The superblock must be validated before any other on-disk region is
accessed using offsets derived from its contents. The implementation
must not trust arbitrary region offsets from an invalid superblock.

## 6. Inode Table

The inode table contains 1024 records of exactly 128 bytes each.

Inode number `i` maps to:

    inode_offset(i) = inode_table_start * block_size + i * 128

The valid inode-number range is 0 through 1023.

An inode records:

- File type and permission mode.
- Owner UID and group GID.
- Link count.
- File size.
- Access, modification, and metadata-change timestamps.
- Flags.
- Eight direct block pointers.
- One single-indirect block pointer.
- Reserved bytes.

Persistent inode fields must be explicitly serialized. The final byte
offsets and widths must add up to exactly 128 bytes.

A block pointer value of zero is not a universal indication of an
unused pointer unless the format explicitly reserves block zero for
metadata and defines zero as the null pointer. The implementation must
define and consistently enforce a null-pointer convention.

Inode 0 is reserved for the root directory.

Supported object types in version 1:

- Free inode.
- Regular file.
- Directory.

Symbolic links, device nodes, sockets, and FIFOs are outside the initial
scope.

## 7. File Block Addressing

Each inode has eight direct pointers and one single-indirect pointer.

Each block pointer is a 32-bit unsigned block number.

A single-indirect block contains:

    4096 / 4 = 1024 block pointers

The theoretical data capacity of an inode is:

    (8 + 1024) * 4096 = 4,227,072 bytes

This is approximately 4.03 MiB.

The single-indirect block itself consumes one allocated data-region
block and is not counted as file payload.

The implementation must reject file operations that exceed the supported
maximum file size. It must also reject invalid, reserved, or out-of-range
block pointers.

Double-indirect and triple-indirect addressing are not part of version 1.

## 8. Allocation Bitmaps

### 8.1 Block bitmap

One bit represents one virtual-disk block.

- Bit value 0: free.
- Bit value 1: allocated or reserved.

The block bitmap covers all 4096 blocks. Metadata-region blocks must be
marked allocated.

Bit numbering is defined as least-significant-bit first within each
byte:

    byte_index = bit_index / 8
    bit_in_byte = bit_index % 8

### 8.2 Inode bitmap

One bit represents one inode.

- Bit value 0: free.
- Bit value 1: allocated.

Inode 0 must be allocated after a successful format because it is the
root directory inode.

### 8.3 Allocation invariants

- A block must never be both free and allocated.
- Metadata blocks must never be returned by the data-block allocator.
- An allocated inode must have its inode bitmap bit set.
- A free inode must not be referenced by a valid directory entry.
- Free counters must agree with bitmap-derived counts.
- Allocation and release operations must validate indices and detect
  double allocation or double release.

## 9. Directory Format

A directory is represented by an inode of directory type. Its contents
are stored in data blocks and contain variable-length directory records.

Each serialized directory record contains:

- Inode number: 4 bytes.
- Record length: 2 bytes.
- Entry type: 1 byte.
- Name length: 1 byte.
- Filename bytes: `name_length` bytes.
- Padding to the required record alignment.

All multi-byte fields use little-endian encoding.

The record length includes the header, filename, and padding. It must be
large enough for the record header and name, aligned as specified by the
implementation, and contained entirely within the directory data block.

Names are limited to 255 bytes and cannot contain a NUL byte or `/`.
The empty name, `.` and `..` have special namespace semantics.

A directory parser must reject malformed records, zero or undersized
record lengths, records extending beyond the block, invalid inode
numbers, and invalid name lengths.

The root directory initially contains:

- `.` referencing inode 0.
- `..` referencing inode 0.

Directory indexing will be implemented as an in-memory optimization.
The directory records remain the persistent source of truth.

## 10. Permissions

Version 1 supports Unix-style owner, group, and other permission bits.

The mode includes file-type information and permission bits. Permission
checks are performed by the virtual filesystem implementation.

The initial implementation supports permission inspection and `chmod`.
Credential handling and access-control semantics must be documented
before enforcing ownership-based read, write, and execute checks.

Host operating-system permissions on the disk-image file are separate
from permissions stored for virtual files.

## 11. Journal

The journal occupies blocks 35 through 98 inclusive, for a total of
64 blocks (256 KiB).

It is intended to support metadata transaction recovery. It is not a
full reproduction of ext4's JBD2 journal.

The journal design must specify:

- Journal header and format version.
- Transaction identifiers.
- Record types and lengths.
- Target-block validation.
- Transaction begin and commit records.
- Checksums or another method for detecting incomplete/corrupt records.
- Replay rules.
- Checkpoint rules.
- Handling of incomplete transactions.
- Journal wraparound and capacity behavior.

The journal must not be considered reliable merely because a commit
record exists in memory. Required journal writes and ordering barriers
must be persisted through the storage layer before reporting success.

Transactions that exceed journal capacity must fail safely or be split
into explicitly defined transactions.

The exact journal record encoding and recovery protocol must be
specified before journal implementation begins.

## 12. Formatting and Mounting

Formatting creates a disk image of exactly 16 MiB and initializes every
filesystem metadata region.

A successful format must:

1. Initialize the superblock.
2. Initialize both bitmaps.
3. Mark reserved metadata blocks allocated.
4. Initialize the inode table.
5. Allocate inode 0.
6. Allocate the root directory's data block.
7. Initialize `.` and `..`.
8. Initialize journal metadata.
9. Persist required structures.
10. Validate the resulting layout.

Mounting validates the superblock and all region boundaries before
accessing other structures. It must reject unsupported versions and
malformed geometry.

The filesystem must not silently format an existing image during mount.

## 13. Consistency Invariants

The consistency checker must eventually validate:

- Superblock geometry and region boundaries.
- Reserved metadata blocks are marked allocated.
- Bitmap sizes and free counters.
- Inode bitmap/table consistency.
- Inode type and size validity.
- Validity of every referenced block.
- Uniqueness of data-block ownership.
- Directory record boundaries and names.
- Directory entries reference allocated inodes.
- Root inode allocation and type.
- Journal record validity and replay safety.
- Link counts where applicable.
- Orphaned allocated inodes and leaked blocks.

Any repair mode must distinguish safe repairs from ambiguous corruption.
It must not silently destroy valid user data to make counters agree.

## 14. Persistence and Crash Recovery

A successful operation must remain visible after clean unmount and
subsequent mount.

Operations involving multiple metadata updates must use a defined
transaction protocol once journaling is implemented.

Fault-injection tests will simulate interruption at selected transaction
stages. Recovery must either complete committed operations or discard
uncommitted operations according to the journal protocol.

The implementation must not claim crash safety until tests cover the
actual persistence ordering and recovery behavior.

## 15. Performance Metrics

The filesystem will expose measurements including:

- Total, used, and free blocks.
- Total, used, and free inodes.
- Storage utilization.
- Block reads and writes.
- Cache hits and misses.
- Directory lookup comparisons and latency.
- Allocation attempts and failures.
- Benchmark throughput and elapsed time.

Metrics must clearly distinguish simulated I/O activity from physical
device performance.

## 16. Format Compatibility

The on-disk format has an explicit version number.

A mount operation must reject an unsupported version rather than
interpreting the image using an incompatible layout.

Changes to persistent field offsets, widths, region layout, allocation
semantics, or journal records require an explicit format-version
decision and corresponding tests.

## 17. Implementation Order

Implementation proceeds in small, testable stages:

1. Constants and checked block-offset calculations.
2. Virtual disk creation, opening, reading, writing, and closing.
3. Superblock serialization, formatting, and validation.
4. Bitmap operations and allocation.
5. Inode serialization and allocation.
6. Directory records and path resolution.
7. File creation, reading, writing, and deletion.
8. Permissions and directory indexing.
9. Block cache and metrics.
10. Journal and recovery.
11. Consistency checker and fault injection.
12. Benchmarks, disk-scheduling simulation, and final validation.

Each stage must compile with strict warnings and have tests before
integration into the next stage.
