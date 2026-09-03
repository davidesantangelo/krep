# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [3.0.2] - 2026-09-03

### Security

- **Bug 1 (print_matching_items newline cache)**: Fixed heap buffer overflow caused by integer multiplication wraparound when allocating the newline-position cache on 32-bit systems ([#44](https://github.com/davidesantangelo/krep/issues/44)).
- **Bug 2 (print_matching_items trailing newline write)**: Fixed heap buffer overflow by accurately simulating formatted line size accounting for duplicate and overlapping multi-pattern matches ([#44](https://github.com/davidesantangelo/krep/issues/44)).
- **Bug 3 (search_file read loop)**: Fixed heap buffer overflow when reading files with apparent sizes exceeding representation limits by safely validating signed `fstat` file size ([#44](https://github.com/davidesantangelo/krep/issues/44)).
- **Bug 4 (gitignore_add_pattern)**: Fixed heap buffer overflow on allocation failure by updating logical capacity only after successful `realloc` and adding integer overflow checks ([#44](https://github.com/davidesantangelo/krep/issues/44)).
- **Bug 5 (simd_sse42_search)**: Fixed heap buffer over-read in SSE4.2 string search for patterns shorter than 16 bytes by reading through a safe bounded zero-padded buffer ([#44](https://github.com/davidesantangelo/krep/issues/44)).

## [3.0.1] - 2026-08-25

### Fixed

- Fixed a busy loop when a zero-width regex match lands exactly at a line ending, including empty regex patterns on platforms that accept them ([#41](https://github.com/davidesantangelo/krep/issues/41)).
- Added bounded CLI regression tests for empty and end-of-line zero-width regex patterns.

## [3.0.0] - 2026-07-01

### Added

- Support for `-r` / `--recursive` directory search with glob and exclude patterns.
- Hidden file inclusion (`--hidden`) and `.gitignore` file support.
- JSON Lines output mode (`--json`).
- Context display (`-A`, `-B`, `-C`).
- Machine-friendly search stats (`--stats`).
