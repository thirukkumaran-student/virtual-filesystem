
#!/bin/sh

set -eu

VFS_BIN="${VFS_BIN:-./build/vfs}"
TEMP_DIR="$(mktemp -d)"
DISK_IMAGE="$TEMP_DIR/test.img"
SESSION1="$TEMP_DIR/session1.log"
SESSION2="$TEMP_DIR/session2.log"

cleanup() {
    rm -rf "$TEMP_DIR"
}

trap cleanup EXIT HUP INT TERM

pass() {
    printf '[PASS] %s\n' "$1"
}

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    exit 1
}

assert_contains() {
    file="$1"
    expected="$2"
    description="$3"

    if grep -Fq "$expected" "$file"; then
        pass "$description"
    else
        printf '\nExpected output: %s\n' "$expected" >&2
        printf '%s\n' '--- Actual output ---' >&2
        cat "$file" >&2
        fail "$description"
    fi
}

printf '%s\n' '=== CLI Integration Tests ==='

# 1. Format a fresh disk image.
if "$VFS_BIN" format "$DISK_IMAGE" >"$TEMP_DIR/format.log" 2>&1; then
    pass "Format a fresh disk image"
else
    cat "$TEMP_DIR/format.log" >&2
    fail "Format a fresh disk image"
fi

assert_contains \
    "$TEMP_DIR/format.log" \
    "Filesystem formatted successfully" \
    "Confirm format success"

# 2. Exercise commands in one CLI session.
if printf '%s\n' \
    'makedir work' \
    'changedir work' \
    'create child.txt' \
    'write child.txt child-data' \
    'back' \
    'removedir work' \
    'changedir work' \
    'delete child.txt' \
    'back' \
    'removedir work' \
    'create sample.txt' \
    'write sample.txt hello world' \
    'append sample.txt !!!' \
    'read sample.txt' \
    'stat sample.txt' \
    'check' \
    'exit' |
    "$VFS_BIN" "$DISK_IMAGE" >"$SESSION1" 2>&1
then
    pass "Execute CLI command sequence"
else
    cat "$SESSION1" >&2
    fail "Execute CLI command sequence"
fi

assert_contains "$SESSION1" \
    "Directory 'work' created successfully" \
    "Create a directory"

assert_contains "$SESSION1" \
    "File 'child.txt' created successfully" \
    "Create a file inside a directory"

assert_contains "$SESSION1" \
    "Directory not empty" \
    "Reject removal of a nonempty directory"

assert_contains "$SESSION1" \
    "File 'child.txt' deleted successfully" \
    "Delete a file"

assert_contains "$SESSION1" \
    "Directory 'work' removed successfully" \
    "Remove the now-empty directory"

assert_contains "$SESSION1" \
    "hello world!!!" \
    "Write, append, and read file contents"

assert_contains "$SESSION1" \
    "File Metadata" \
    "Display file metadata"

assert_contains "$SESSION1" \
    "Result: CONSISTENT" \
    "Pass the filesystem consistency check"

# 3. Reopen the same image to test persistence.
if printf '%s\n' \
    'read sample.txt' \
    'list' \
    'check' \
    'exit' |
    "$VFS_BIN" "$DISK_IMAGE" >"$SESSION2" 2>&1
then
    pass "Reopen the disk image"
else
    cat "$SESSION2" >&2
    fail "Reopen the disk image"
fi

assert_contains "$SESSION2" \
    "hello world!!!" \
    "Preserve file contents across sessions"

assert_contains "$SESSION2" \
    "sample.txt" \
    "Preserve directory entries across sessions"

assert_contains "$SESSION2" \
    "Result: CONSISTENT" \
    "Remain consistent after reopening"

printf '\nAll CLI integration tests passed.\n'
