# krep - A high-performance string search utility

![Version](https://img.shields.io/badge/version-0.1.7-blue)
![License](https://img.shields.io/badge/license-BSD-green)

`krep` is a blazingly fast string search utility designed for performance-critical applications. It implements multiple optimized search algorithms and leverages modern hardware capabilities to deliver maximum throughput.

## Blog Post

Building a High-Performance String Search Utility

https://dev.to/daviducolo/introducing-krep-building-a-high-performance-string-search-utility-2pdo

## Features

- **Multiple optimized search algorithms**
  - Boyer-Moore-Horspool algorithm for general-purpose efficient pattern matching
  - Knuth-Morris-Pratt (KMP) algorithm optimized for short patterns
  - Rabin-Karp algorithm for longer pattern lengths
  - SIMD acceleration on compatible hardware (SSE4.2, AVX2, and ARM NEON)

- **Maximum performance**
  - Memory-mapped file I/O for optimal throughput
  - Multi-threaded parallel search for large files
  - Automatic algorithm selection based on pattern characteristics and hardware capabilities

- **Flexible search options**
  - Case-sensitive and case-insensitive matching
  - Direct string search in addition to file search
  - Match counting mode

## Installation

### From Source

```bash
git clone https://github.com/davidesantangelo/krep.git
cd krep
make
sudo make install
```

### Prerequisites

- GCC or Clang compiler
- POSIX-compliant system (Linux, macOS, BSD)
- pthread library

## Usage

```bash
krep [OPTIONS] PATTERN [FILE]
```

### Examples

Search for "error" in a log file:
```bash
krep "error" system.log
```

Case-insensitive search with 8 threads:
```bash
krep -i -t 8 "ERROR" large_logfile.log
```

Count occurrences without displaying matching lines:
```bash
krep -c "TODO" *.c
```

Search within a string instead of a file:
```bash
krep -s "Hello" "Hello world"
```

## Command Line Options

- `-i` Case-insensitive search
- `-c` Count matches only (don't print matching lines)
- `-t NUM` Use NUM threads (default: 4)
- `-s STRING` Search within STRING instead of a file
- `-v` Display version information
- `-h` Display help message

## Performance

`krep` is designed with performance as a primary goal:

- **Memory-mapped I/O**: Avoids costly read() system calls
- **Optimized algorithms**: Uses multiple string-matching algorithms optimized for different scenarios
- **SIMD acceleration**: Utilizes SSE4.2, AVX2, or ARM Neon when available
- **Multi-threading**: Processes large files in parallel chunks
- **Minimal allocations**: Reduces memory overhead and fragmentation

## Benchmarks

Performance compared to standard tools (searching a 1GB text file for a common pattern):

| Tool | Time (seconds) | Speed (MB/s) |
|------|----------------|--------------|
| krep | 0.78 | 1,282 |
| grep | 2.95 | 339 |
| ripgrep | 1.48 | 676 |

*Note: Performance may vary based on hardware, file characteristics, and search pattern.*

## How It Works

`krep` uses several strategies to achieve high performance:

1. **Intelligent algorithm selection**: Automatically chooses the optimal algorithm based on pattern characteristics:
   - KMP for very short patterns (< 3 characters)
   - SIMD/AVX2 for medium-length patterns (when hardware supports it)
   - Boyer-Moore for medium-length patterns (when SIMD is unavailable)
   - Rabin-Karp for longer patterns (> 16 characters)

2. **Parallelization strategy**: For files larger than 1MB, splits the search into chunks and processes them concurrently

3. **Memory efficiency**: Uses memory-mapped I/O to leverage the operating system's page cache

4. **Hardware acceleration**: Automatically detects and utilizes SSE4.2, AVX2 and ARM Neon instructions when available

## Testing

krep includes a comprehensive test suite to validate its functionality. To run the tests:

```bash
# From the project root directory
make test
```

This will compile and execute the test suite, which verifies:
- Basic search functionality for all algorithms
- Edge cases (empty strings, single characters)
- Case sensitivity handling
- Repeated pattern handling
- Performance benchmarks

### Example test output:

```
Running krep tests...

=== Basic Search Tests ===
✓ PASS: Boyer-Moore finds 'quick' once
✓ PASS: Boyer-Moore finds 'fox' once
✓ PASS: Boyer-Moore doesn't find 'cat'
// ...more test results...

=== Test Summary ===
Tests passed: 23
Tests failed: 0
Total tests: 23
```

## The Story Behind the Name

The name "krep" has an interesting origin. It is inspired by the Icelandic word "kreppan," which means "to grasp quickly" or "to catch firmly." I came across this word while researching efficient techniques for pattern recognition.

Just as skilled fishers identify patterns in the water to locate fish quickly, I designed "krep" to find patterns in text with maximum efficiency. The name is also short and easy to remember—perfect for a command-line utility that users might type hundreds of times per day.
## Author

- **Davide Santangelo** - [GitHub](https://github.com/davidesantangelo)

## License

This project is licensed under the BSD-2 License - see the LICENSE file for details.

Copyright © 2025 Davide Santangelo
