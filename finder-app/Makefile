# Makefile for finder-app

# Use Buildroot's cross-compiler when provided
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g

# Default target
all: writer

# Build writer using whatever compiler Buildroot passes in (or gcc if not cross-compiling)
writer: writer.c
	$(CC) $(CFLAGS) -o writer writer.c

# Clean target for both host and cross builds
clean:
	rm -f writer

