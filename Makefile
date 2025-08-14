# SPDX-FileCopyrightText: 2025 Rivos Inc.
#
# SPDX-License-Identifier: Apache-2.0

# Compiler and flags
CC := clang
CFLAGS := -v -target aarch64-linux-android34 -march=armv8.5-a+memtag -static -O0

# Source files
SRC := main.c

# Object files
OBJS := $(SRC:.c=.o)

# Target executable
TARGET := mte_bm

# Default target
all: $(TARGET)

# Linking step
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# Compilation step for each .c file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)