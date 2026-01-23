# krep - A high-performance string search utility
# Author: Davide Santangelo
# Version: 1.5.0

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

CC = gcc
CFLAGS = -Wall -Wextra -O3 -ffast-math -std=c11 -pthread -D_GNU_SOURCE -D_DEFAULT_SOURCE \
         -flto -funroll-loops -finline-functions
LDFLAGS = -pthread -flto

# Build mode: set NATIVE=1 for maximum performance on local machine
# Example: make NATIVE=1
ifdef NATIVE
    CFLAGS += -march=native -mtune=native
endif

# Detect architecture for SIMD flags
ARCH := $(shell uname -m)

ifeq ($(ARCH), x86_64)
    # Check for AVX-512 support (Linux: /proc/cpuinfo, macOS: sysctl)
    HAS_AVX512 := $(shell (grep -q avx512f /proc/cpuinfo 2>/dev/null && echo 1) || \
                          (sysctl -n machdep.cpu.features 2>/dev/null | grep -q AVX512F && echo 1) || \
                          echo 0)
    # Check for AVX2 support
    HAS_AVX2 := $(shell (grep -q avx2 /proc/cpuinfo 2>/dev/null && echo 1) || \
                        (sysctl -n machdep.cpu.features 2>/dev/null | grep -q AVX2 && echo 1) || \
                        (sysctl -n machdep.cpu.leaf7_features 2>/dev/null | grep -q AVX2 && echo 1) || \
                        echo 0)
    
    # Enable the best available SIMD instruction set
    ifeq ($(HAS_AVX512), 1)
        CFLAGS += -mavx512f -mavx512bw -msse4.2 -mavx2
    else ifeq ($(HAS_AVX2), 1)
        CFLAGS += -mavx2 -msse4.2
    else
        # Fallback to SSE4.2 which is widely supported on x86_64
        CFLAGS += -msse4.2
    endif
else ifeq ($(ARCH), arm64)
    # Enable NEON for arm64 (Apple Silicon, etc.)
    CFLAGS += -D__ARM_NEON
    # Note: GCC might enable NEON automatically on arm64, but explicit flag is safer
else ifeq ($(ARCH), aarch64)
    # Enable NEON for aarch64 Linux
    CFLAGS += -D__ARM_NEON
endif

# Source files
SRCS = krep.c aho_corasick.c
OBJS = $(SRCS:.c=.o)

# Test source files
TEST_SRCS = test/test_krep.c test/test_regex.c test/test_multiple_patterns.c
TEST_OBJS_MAIN = krep_test.o aho_corasick_test.o # Specific objects for test build
TEST_OBJS_TEST = $(TEST_SRCS:.c=.o)
TEST_TARGET = krep_test

TARGET = krep

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Rule for main objects
%.o: %.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Test Build ---
# Rule for test-specific main objects (compiled with -DTESTING)
krep_test.o: krep.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -DTESTING -c krep.c -o krep_test.o

aho_corasick_test.o: aho_corasick.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -DTESTING -c aho_corasick.c -o aho_corasick_test.o

# Rule for test file objects (compiled with -DTESTING)
test/%.o: test/%.c test/test_krep.h test/test_compat.h krep.h
	$(CC) $(CFLAGS) -DTESTING -c $< -o $@

# Link test executable
$(TEST_TARGET): $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST)
	$(CC) $(CFLAGS) -DTESTING -o $(TEST_TARGET) $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST) $(LDFLAGS) -lm # Add -lm if needed

test: $(TEST_TARGET)
	./$(TEST_TARGET)

test_directory: test/test_directory.c krep.c aho_corasick.c
	$(CC) $(CFLAGS) -DTESTING -o $@ $^ $(LDFLAGS)

all-tests: test_basic test_krep test_regex test_multiple_patterns test_directory

# --- Installation ---
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# --- Cleanup ---
clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJS) $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST) *.o test/*.o
