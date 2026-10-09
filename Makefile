CC := gcc

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS :=
LDLIBS :=

BUILD_DIR := build

ALL_SOURCES := $(wildcard src/*.c)
CORE_SOURCES := $(filter-out src/main.c,$(ALL_SOURCES))
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINARIES := $(patsubst tests/test_%.c,$(BUILD_DIR)/test_%,$(TEST_SOURCES))

.PHONY: all test clean

all: $(BUILD_DIR)/vfs

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/vfs: $(ALL_SOURCES) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ALL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(CORE_SOURCES) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@


test: $(BUILD_DIR)/vfs $(TEST_BINARIES)
	@set -e; \
	for test_binary in $(TEST_BINARIES); do \
		echo ""; \
		echo "Running $$test_binary"; \
		./$$test_binary; \
	done
	@echo ""; \
	echo "Running CLI integration tests"; \
	./tests/test_cli.sh


clean:
	rm -rf $(BUILD_DIR)