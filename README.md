# k(r)ep - A high-performance string search utility

![Version](https://img.shields.io/badge/version-3.0.0-blue)
![License](https://img.shields.io/badge/license-BSD-green)

`krep` is an optimized string search utility designed for maximum throughput, low-latency feedback, and modern command-line ergonomics when processing large files and source trees. It combines mmap-based I/O, adaptive algorithms, SIMD acceleration where available, multi-pattern search, recursive traversal controls, JSON Lines output, contextual display, and machine-friendly stats.

Version 3.0 moves krep from a minimal fast scanner to a practical daily search CLI: fast by default, scriptable when needed, and comfortable in source trees with globs, exclusions, hidden-file control, `.gitignore` support, file listing modes, and quiet checks.

## The Story Behind the Name

The name "krep" has an interesting origin. It is inspired by the Icelandic word "kreppan," which means "to grasp quickly" or "to catch firmly." I came across this word while researching efficient techniques for pattern recognition.

Just as skilled fishers identify patterns in the water to locate fish quickly, I designed "krep" to find patterns in text with maximum efficiency. The name is also short and easy to remember—perfect for a command-line utility that users might type hundreds of times per day.

## Key Features

- **Multiple search algorithms**: Boyer-Moore-Horspool, KMP, Two-Way, Shift-Or/BNDM, Aho-Corasick for optimal performance across different pattern types
- **Algorithm selection**: Automatic smart selection with optional `--algo` override for fine-tuning
- **SIMD acceleration**: Uses SSE4.2, AVX2, or NEON instructions when available for blazing-fast searches
- **Memory-mapped I/O**: Maximizes throughput when processing large files
- **Multi-threaded search**: Automatically parallelizes searches across available CPU cores
- **Regex support**: POSIX Extended Regular Expression searching
- **Multiple pattern search**: Efficiently search for multiple patterns simultaneously using Aho-Corasick
- **Recursive directory search**: Skip binary files and common non-code directories
- **Modern tree filtering**: Include or exclude paths with repeatable `--glob` and `--exclude`
- **Gitignore support**: Respect `.gitignore` files during recursive search with `--gitignore`
- **Hidden path control**: Search hidden files and directories with `--hidden`
- **Stdin pattern input**: Read patterns from stdin with `-f -` for seamless pipeline integration
- **Context and line numbers**: `-n`, `-A`, `-B`, and `-C` for grep-compatible surrounding context
- **JSON Lines output**: `--json` / `--jsonl` for editors, dashboards, and automation
- **File listing modes**: `-l`, `-L`, and `-q` for fast shell checks and CI workflows
- **Search stats**: `--stats` emits a compact stderr summary of searched files, skipped paths, bytes, matches, and elapsed time
- **Colored output**: Highlights matches for better readability
- **Refined terminal UI**: Clearer colors, improved `-o` line index styling, and a redesigned help screen
- **Specialized algorithms**: Optimized handling for single-character and short patterns
- **Match Limiting**: Stop searching a file after a specific number of matching lines are found.

## What's New in 3.0

- JSON Lines output with structured records for matching lines, only-matching records, counts, and file lists
- Grep-compatible context controls with `-A`, `-B`, `-C`, plus `-n` line numbering
- Recursive include/exclude filtering with repeatable `--glob` and `--exclude`
- `--hidden`, `-l`, `-L`, `-q`, and `--stats` for modern scripting and repository workflows
- Default recursive target of `.` when using `-r` without an explicit directory
- CI now includes a CLI integration suite covering the new 3.0 user-facing behavior
- **Shift-Or / BNDM** bit-parallel search for lightning-fast short pattern matching (2–8 bytes)
- **Two-Way** string matching (O(n+m) worst-case) as the new default scalar fallback
- **NEON 32-byte processing** on ARM64/Apple Silicon for doubled throughput
- **Fast word-char lookup table** replacing `isalnum()` in hot paths
- **MADV_HUGEPAGE** support on Linux for reduced TLB pressure on large files
- **Double-distance prefetching** for better cache line utilization
- New `--algo bndm` and `--algo two` options for manual algorithm selection

## Installation

### Using Homebrew (macOS)

If you are on macOS and have Homebrew installed, you can install `krep` easily:

```bash
brew install krep
```

### Building from Source

```bash
# Clone the repository
git clone https://github.com/davidesantangelo/krep.git
cd krep

# Build and install
make
sudo make install

# uninstall
sudo make uninstall
```

The binary will be installed to `/usr/local/bin/krep` by default.

### Requirements

- GCC or compatible C compiler
- POSIX-compliant system (Linux, macOS, BSD)
- pthread support

### Build Options

Override default optimization settings in the Makefile:

```bash
# Disable architecture-specific optimizations
make ENABLE_ARCH_DETECTION=0
```

## Usage

```bash
krep [OPTIONS] PATTERN [FILE | DIRECTORY]
krep [OPTIONS] -e PATTERN [FILE | DIRECTORY]
krep [OPTIONS] -f FILE [FILE | DIRECTORY]
krep [OPTIONS] -s PATTERN STRING_TO_SEARCH
krep [OPTIONS] PATTERN < FILE
cat FILE | krep [OPTIONS] PATTERN
echo 'pattern' | krep -f - [FILE | DIRECTORY]
```

## Usage Examples

Search for a fixed string in a file:

```bash
krep -F "value: 100%" config.ini
```

Search recursively:

```bash
krep -r "function" ./project
```

Search recursively respecting `.gitignore`:

```bash
krep -r --gitignore "TODO" ./project
```

Search only C sources while excluding generated/vendor paths:

```bash
krep -r --gitignore --glob '*.c' --glob '*.h' --exclude '*/vendor/*' "TODO" .
```

Show line numbers and surrounding context:

```bash
krep -n -C 2 "panic" app.log
```

Emit JSON Lines for automation:

```bash
krep --json -n "needle" ./src/main.c
```

List files with or without matches:

```bash
krep -r -l "DeprecatedAPI" .
krep -r -L "license header" --glob '*.c' .
```

Run a quiet existence check:

```bash
krep -q "required_setting" config.ini
```

Read patterns from stdin (pipe-friendly):

```bash
echo 'pattern' | krep -f - target.txt
```

Whole word search (matches only complete words):

```bash
krep -w 'cat' samples/text.en
```

Use with piped input:

```bash
cat krep.c | krep 'c'
```

## Command Line Options

- `-i, --ignore-case` Case-insensitive search
- `-c, --count` Count matching lines only
- `-o, --only-matching` Print only the matched parts of lines
- `-n, --line-number` Prefix matching lines with line numbers
- `-A NUM, --after-context=NUM` Print NUM lines after each matching line
- `-B NUM, --before-context=NUM` Print NUM lines before each matching line
- `-C NUM, --context=NUM` Print NUM lines before and after each matching line
- `-l, --files-with-matches` Print only file names that contain matches
- `-L, --files-without-match` Print only file names that do not contain matches
- `-q, --quiet` Suppress output and use only the exit status
- `-e PATTERN, --pattern=PATTERN` Specify pattern(s). Can be used multiple times.
- `-f FILE, --file=FILE` Read patterns from FILE, one per line. Use `-` for stdin.
- `-m NUM, --max-count=NUM` Stop searching each file after finding NUM matching lines.
- `-E, --extended-regexp` Use POSIX Extended Regular Expressions
- `-F, --fixed-strings` Interpret pattern as fixed string(s) (default unless -E is used)
- `-r, --recursive` Recursively search directories
- `--glob=GLOB` Include only files matching GLOB. Can be used multiple times.
- `--exclude=GLOB` Exclude paths matching GLOB. Can be used multiple times.
- `--hidden` Include hidden files and directories during recursive search
- `--gitignore` Respect `.gitignore` files during recursive search
- `--algo=ALGO` Force search algorithm: `auto` (default), `bm` (Boyer-Moore), `kmp` (KMP), `bndm` (Shift-Or), `two` (Two-Way)
- `-t NUM, --threads=NUM` Use NUM threads for file search (default: auto)
- `-s STRING, --string=STRING` Search in the provided STRING instead of file(s)
- `-w, --word-regexp` Match only whole words
- `--json, --jsonl` Emit JSON Lines output
- `--stats` Print a compact search summary to stderr
- `--color[=WHEN]` Control color output ('always', 'never', 'auto')
- `--no-simd` Explicitly disable SIMD acceleration
- `-v, --version` Show version information
- `-h, --help` Show help message

## Performance Benchmarks

Benchmarks are run with the official dataset:

```bash
curl -LO 'https://burntsushi.net/stuff/subtitles2016-sample.en.gz'
gzip -dk subtitles2016-sample.en.gz
```

You can reproduce the `krep` vs `ripgrep` comparison with:

```bash
make bench-rg
# optional: RUNS=7 bash test/benchmark_krep_vs_rg.sh Sherlock
```

### krep vs ripgrep / grep (warm cache, 7 runs average baseline)

| Pattern | krep avg real (s) | ripgrep avg real (s) | GNU grep real (s) | krep vs rg | krep vs grep |
| --- | ---: | ---: | ---: | ---: | ---: |
| `the` | 0.132857 | 0.318571 | 4.848571 | 2.40x | 36.49x |
| `Sherlock` | 0.031429 | 0.080000 | 2.777143 | 2.55x | 88.36x |

_Measured on macOS ARM64 with the official `subtitles2016-sample.en` dataset. `krep` 3.0.0 keeps the 2.4.0 high-throughput engine and adds modern CLI output, filtering, and automation controls. Results vary by CPU, storage, cache state, compiler, and workload._

## How Krep Works

Krep achieves its high performance through several key techniques:

### 1. Smart Algorithm Selection

Krep automatically selects the optimal search algorithm based on the pattern and available hardware:

- **Boyer-Moore-Horspool** for most literal string searches
- **Two-Way** algorithm for general scalar fallback (O(n+m) worst-case)
- **Shift-Or / BNDM** bit-parallel search for short patterns (2–8 bytes)
- **Knuth-Morris-Pratt (KMP)** for very short patterns and repetitive patterns
- **memchr optimization** for single-character patterns
- **SIMD Acceleration** (SSE4.2, AVX2, or NEON) for compatible hardware
- **Regex Engine** for regular expression patterns
- **Aho-Corasick** for efficient multiple pattern matching (auto-selected with multiple `-e` patterns)

Use `--algo=bm`, `--algo=kmp`, `--algo=bndm`, or `--algo=two` to override the automatic selection for single-pattern literal searches.

### 2. Multi-threading Architecture

Krep utilizes parallel processing to dramatically speed up searches:

- Automatically detects available CPU cores
- Divides large files into chunks for parallel processing
- Implements thread pooling for maximum efficiency
- Optimized thread count selection based on file size
- Careful boundary handling to ensure no matches are missed

### 3. Memory-Mapped I/O

Instead of traditional read operations:

- Memory maps files for direct access by the CPU
- Significantly reduces I/O overhead
- Enables CPU cache optimization
- Progressive prefetching for larger files

### 4. Optimized Data Structures

- Zero-copy architecture where possible
- Efficient match position tracking
- Lock-free aggregation of results

### 5. Skipping Non-Relevant Content

When using recursive search (`-r`), Krep automatically:

- Skips common binary file types
- Ignores version control directories (`.git`, `.svn`)
- Bypasses dependency directories (`node_modules`, `venv`)
- Detects binary content to avoid searching non-text files
- Applies repeatable `--glob` include filters and `--exclude` path filters
- Can include hidden paths with `--hidden`
- Can emit structured JSON Lines with `--json` for downstream tools

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Author

- **Davide Santangelo** - [GitHub](https://github.com/davidesantangelo)

## License

This project is licensed under the BSD-2 License - see the LICENSE file for details.

Copyright © 2025 Davide Santangelo
