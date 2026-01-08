/* krep - A high-performance string search utility
 *
 * Author: Davide Santangelo
 * Year: 2025
 *
 */

// Define _GNU_SOURCE to potentially enable MAP_POPULATE and memrchr
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "krep.h"         // Include the header file
#include "aho_corasick.h" // Include AC header for build/free functions

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h> // Include for mmap, madvise constants
#include <pthread.h>
#include <inttypes.h> // For PRIu64 macro
#include <errno.h>
#include <limits.h>    // For SIZE_MAX, PATH_MAX
#include <regex.h>     // For POSIX regex support
#include <dirent.h>    // For directory operations
#include <sys/types.h> // For mode_t, DIR*, struct dirent
#include <getopt.h>    // For command-line parsing
#include <stdatomic.h> // For atomic operations in multithreading

// Add forward declaration for is_repetitive_pattern here
static bool is_repetitive_pattern(const char *pattern, size_t pattern_len);

// Forward declaration for ensure_line_buffer_capacity
static bool ensure_line_buffer_capacity(char **buffer_ptr, size_t *capacity_ptr, size_t current_pos, size_t needed);

// SIMD Intrinsics Includes based on compiler flags (from Makefile)
#if defined(__AVX512F__) && defined(__AVX512BW__)
#include <immintrin.h> // AVX-512 intrinsics
#define KREP_USE_AVX512 1
#define KREP_USE_AVX2 1
#define KREP_USE_SSE42 1
#elif defined(__AVX2__)
#include <immintrin.h> // AVX2 intrinsics
#define KREP_USE_AVX512 0
#define KREP_USE_AVX2 1
#define KREP_USE_SSE42 1
#else
#define KREP_USE_AVX512 0
#define KREP_USE_AVX2 0
#endif

#if defined(__SSE4_2__) && !KREP_USE_AVX2
#include <nmmintrin.h> // SSE4.2 intrinsics
#define KREP_USE_SSE42 1
#elif !defined(KREP_USE_SSE42)
#define KREP_USE_SSE42 0
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h> // NEON intrinsics
#define KREP_USE_NEON 1
#else
#define KREP_USE_NEON 0
#endif

// Constants
#define MAX_PATTERN_LENGTH 1024
#define DEFAULT_THREAD_COUNT 0
#define MIN_CHUNK_SIZE (2 * 1024 * 1024)        // Reduced for better parallelism
#define LARGE_FILE_THRESHOLD (64 * 1024 * 1024) // 64MB threshold for advanced optimizations
#define SINGLE_THREAD_FILE_SIZE_THRESHOLD MIN_CHUNK_SIZE
#define ADAPTIVE_THREAD_FILE_SIZE_THRESHOLD 0
#define VERSION "1.4.2"
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BINARY_CHECK_BUFFER_SIZE 1024  // Bytes to check for binary content
#define MAX_PATTERN_FILE_LINE_LEN 2048 // Max length for a pattern line read from file
#define CACHE_LINE_SIZE 64             // Modern CPU cache line size
#define PREFETCH_DISTANCE 512          // Bytes ahead to prefetch

// Compiler hints for better optimization
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define HOT_FUNCTION __attribute__((hot))
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))

// Determine max pattern length usable by SIMD based on highest available instruction set
// NOTE: Current SIMD implementations only support case-sensitive search.
#if KREP_USE_AVX512
// AVX-512 implementation handles <= 64 bytes.
const size_t SIMD_MAX_PATTERN_LEN = 64;
#elif KREP_USE_AVX2
// AVX2 implementation handles <= 32 bytes.
const size_t SIMD_MAX_PATTERN_LEN = 32;
#elif KREP_USE_SSE42
const size_t SIMD_MAX_PATTERN_LEN = 16;
#elif KREP_USE_NEON
const size_t SIMD_MAX_PATTERN_LEN = 16;
#else
const size_t SIMD_MAX_PATTERN_LEN = 0;
#endif

// Global state (Consider encapsulating if becomes too large)
static bool color_output_enabled KREP_UNUSED = false;
static bool only_matching = false; // -o flag
static bool force_no_simd = false;
static atomic_bool global_match_found_flag = false; // Used in recursive search
static atomic_bool madvise_warning_emitted = false;   // Suppress repeated madvise warnings

// Global lookup table for fast lowercasing
unsigned char lower_table[256]; // Remove static

// Initialize the lower_table at program start
static void __attribute__((constructor)) init_lower_table(void)
{
    for (int i = 0; i < 256; i++)
    {
        lower_table[i] = tolower(i);
    }
}

// --- Match Result Management ---

// Initialize match result structure
match_result_t *match_result_init(uint64_t initial_capacity)
{
    match_result_t *result = malloc(sizeof(match_result_t));
    if (!result)
    {
        perror("malloc failed for match_result_t");
        return NULL;
    }

    // Check for overflow cases before allocating memory for positions
    if (initial_capacity == 0)
    {
        initial_capacity = 16; // Default initial size
    }
    else if (initial_capacity > SIZE_MAX / sizeof(match_position_t))
    {
        // This allocation would overflow, refuse to proceed
        fprintf(stderr, "Error: Requested capacity too large for match_result_init\n");
        free(result);
        return NULL;
    }

    result->positions = malloc(initial_capacity * sizeof(match_position_t));
    if (!result->positions)
    {
        perror("malloc failed for match positions array");
        free(result);
        return NULL;
    }

    result->count = 0;
    result->capacity = initial_capacity;
    return result;
}

// Add a match to the result structure, reallocating if necessary
inline bool match_result_add(match_result_t *result, size_t start_offset, size_t end_offset)
{
    if (!result)
        return false;

    // Check if we need to expand the capacity
    if (result->count >= result->capacity)
    {
        // Fast path for initial allocation
        if (result->capacity == 0)
        {
            size_t initial_capacity = 16;
            result->positions = malloc(initial_capacity * sizeof(match_position_t));
            if (!result->positions)
            {
                perror("Error allocating initial match positions array");
                return false;
            }
            result->capacity = initial_capacity;
        }
        else
        {
            // Calculate new capacity with overflow protection
            uint64_t new_capacity;

            // Check for potential overflow in capacity doubling
            if (result->capacity > SIZE_MAX / (2 * sizeof(match_position_t)))
            {
                // Try to allocate maximum safe capacity if doubling would overflow
                new_capacity = SIZE_MAX / sizeof(match_position_t);

                // If we can't grow further, signal failure
                if (new_capacity <= result->capacity)
                {
                    fprintf(stderr, "Error: Cannot increase result capacity further (at %" PRIu64 " matches).\n",
                            result->capacity);
                    return false;
                }
            }
            else
            {
                // Normal doubling strategy for growth
                new_capacity = result->capacity * 2;
            }

            // Perform the reallocation
            match_position_t *new_positions = realloc(result->positions,
                                                      new_capacity * sizeof(match_position_t));
            if (!new_positions)
            {
                perror("Error reallocating match positions array");
                // Existing array is preserved by realloc semantics
                return false;
            }

            result->positions = new_positions;
            result->capacity = new_capacity;
        }
    }

    // Add the new match position
    result->positions[result->count].start_offset = start_offset;
    result->positions[result->count].end_offset = end_offset;
    result->count++;

    return true;
}

// Free memory associated with match result structure
void match_result_free(match_result_t *result)
{
    if (!result)
        return;
    if (result->positions)
        free(result->positions);
    free(result);
}

// Merge results from a source list into a destination list
// Assumes destination has enough capacity (caller must ensure or realloc)
// Adjusts offsets from source based on chunk_offset
bool match_result_merge(match_result_t *dest, const match_result_t *src, size_t chunk_offset)
{
    if (!dest || !src || src->count == 0)
        return true; // Nothing to merge or invalid input

    // Ensure destination has enough capacity
    uint64_t required_capacity = dest->count + src->count;
    if (required_capacity < dest->count)
    { // Check for overflow
        fprintf(stderr, "Error: Required capacity overflow during merge.\n");
        return false;
    }

    if (required_capacity > dest->capacity)
    {
        // Prevent potential integer overflow during capacity calculation
        uint64_t new_capacity = dest->capacity;
        if (new_capacity == 0)
            new_capacity = 16;
        while (new_capacity < required_capacity)
        {
            // Check for potential overflow before doubling
            if (new_capacity > SIZE_MAX / (2 * sizeof(match_position_t)))
            {
                new_capacity = required_capacity; // Try exact size
                if (new_capacity < required_capacity)
                { // Check again
                    fprintf(stderr, "Error: Cannot allocate sufficient capacity for merge (overflow).\n");
                    return false;
                }
                break; // Use exact required capacity
            }
            new_capacity *= 2;
            // Handle case where doubling overflows but required_capacity is still reachable
            if (new_capacity < dest->capacity)
            {
                new_capacity = required_capacity;
                if (new_capacity < required_capacity)
                {
                    fprintf(stderr, "Error: Cannot allocate sufficient capacity for merge (overflow 2).\n");
                    return false;
                }
                break;
            }
        }
        // Final check if required_capacity itself is too large
        if (new_capacity < required_capacity)
        {
            fprintf(stderr, "Error: Cannot allocate sufficient capacity for merge (required > new).\n");
            return false;
        }

        match_position_t *new_positions = realloc(dest->positions, new_capacity * sizeof(match_position_t));
        if (!new_positions)
        {
            perror("Error reallocating destination match positions for merge");
            return false;
        }
        dest->positions = new_positions;
        dest->capacity = new_capacity;
    }

    // Copy and adjust offsets
    for (uint64_t i = 0; i < src->count; ++i)
    {
        dest->positions[dest->count].start_offset = src->positions[i].start_offset + chunk_offset;
        dest->positions[dest->count].end_offset = src->positions[i].end_offset + chunk_offset;
        dest->count++;
    }
    return true;
}

// Merge results applying a hard cap on the number of elements copied from src
static bool match_result_merge_limited(match_result_t *dest,
                                       const match_result_t *src,
                                       size_t chunk_offset,
                                       uint64_t limit)
{
    if (!dest || !src || src->count == 0 || limit == 0)
        return true;

    uint64_t copy_count = src->count;
    if (limit < copy_count)
        copy_count = limit;

    if (copy_count == src->count)
    {
        return match_result_merge(dest, src, chunk_offset);
    }

    for (uint64_t i = 0; i < copy_count; ++i)
    {
        if (!match_result_add(dest,
                              src->positions[i].start_offset + chunk_offset,
                              src->positions[i].end_offset + chunk_offset))
        {
            return false;
        }
    }

    return true;
}

// --- Line Finding Functions ---

// Find the start of the line containing the given position
// Uses memrchr (GNU extension) if available for potential speedup, otherwise manual loop.
size_t find_line_start(const char *text, size_t max_len, size_t pos)
{
    if (pos > max_len)
        pos = max_len; // Ensure pos is within bounds

    if (pos == 0)
        return 0; // Already at the start

// Check if memrchr is likely available (common on Linux/glibc)
#if defined(_GNU_SOURCE) && !defined(__APPLE__) && !defined(_WIN32) // Crude check, refine if needed
    // Use memrchr to find the last newline before or at pos-1
    const char *start_ptr = text;
    // memrchr searches backwards from text + pos - 1 for 'pos' bytes
    size_t search_len = pos;
    void *newline_ptr = memrchr(start_ptr, '\n', search_len);

    if (newline_ptr != NULL)
    {
        // Found a newline, the line starts *after* it
        return (const char *)newline_ptr - start_ptr + 1;
    }
    else
    {
        // No newline found before pos, so the line starts at the beginning of the text
        return 0;
    }
#else
    // Fallback to manual loop if memrchr is not available or not detected
    size_t current = pos;
    while (current > 0 && text[current - 1] != '\n')
    {
        current--;
    }
    return current;
#endif
}

// Find the end of the line containing the given position
size_t find_line_end(const char *text, size_t text_len, size_t pos)
{
    if (pos >= text_len)
        return text_len; // Already at or past the end

    const char *newline_ptr = memchr(text + pos, '\n', text_len - pos);
    return (newline_ptr == NULL) ? text_len : (size_t)(newline_ptr - text);

    // Original loop kept for reference:
    // while (pos < text_len && text[pos] != '\n')
    // {
    //     pos++;
    // }
    // return pos; // Returns index of '\n' or text_len if no newline found
}

// --- Printing Function ---

// Comparison function for qsort on match_position_t by start_offset
static int compare_match_positions(const void *a, const void *b)
{
    const match_position_t *pa = (const match_position_t *)a;
    const match_position_t *pb = (const match_position_t *)b;
    if (pa->start_offset < pb->start_offset)
        return -1;
    if (pa->start_offset > pb->start_offset)
        return 1;
    // Secondary sort by end offset if starts are equal (optional, but can be useful)
    if (pa->end_offset < pb->end_offset)
        return -1;
    if (pa->end_offset > pb->end_offset)
        return 1;
    return 0;
}

// Helper function to safely append data to a batch buffer
// Modifies the current write pointer and batch position pointer
static inline void safe_append_to_batch(char **current_write_ptr_ptr, char *batch_buffer_end, size_t *batch_pos_ptr, size_t batch_buffer_size, const char *data, size_t data_len)
{
    char *current_write_ptr = *current_write_ptr_ptr;
    size_t available_space = batch_buffer_end - current_write_ptr;

    if (data_len <= available_space)
    {
        memcpy(current_write_ptr, data, data_len);
        *current_write_ptr_ptr += data_len; // Update the caller's pointer
    }
    else
    {
        // Handle buffer overflow scenario (truncate)
        if (available_space > 0)
        {
            memcpy(current_write_ptr, data, available_space);
            *current_write_ptr_ptr += available_space; // Update the caller's pointer
        }
        // Mark buffer as full by setting the position to the size
        *batch_pos_ptr = batch_buffer_size;
    }
}

size_t print_matching_items(const char *filename, const char *text, size_t text_len, const match_result_t *result, const search_params_t *params)
{
    // Basic validation: No results, no text, or zero matches means nothing to print.
    if (!result || !text || result->count == 0)
        return 0;

    size_t items_printed_count = 0;
    size_t max_count = params->max_count; // Get max_count from params

    // Get global configuration values
    extern bool only_matching;        // External variable declared in krep.h
    extern bool color_output_enabled; // External variable declared in krep.h

// --- Setup enhanced buffering ---
// Use a larger stdout buffer than default to reduce syscalls
#define STDOUT_BUFFER_SIZE (8 * 1024 * 1024) // 8MB stdout buffer
    static char stdout_buf[STDOUT_BUFFER_SIZE];
    static bool stdout_buffer_initialized = false;
    if (!stdout_buffer_initialized)
    {
        setvbuf(stdout, stdout_buf, _IOFBF, STDOUT_BUFFER_SIZE);
        stdout_buffer_initialized = true;
    }

// --- Preallocate reusable line buffer for formatting ---
#define LINE_BUFFER_INITIAL_SIZE (512 * 1024) // Start with 512KB
    char *line_buffer = malloc(LINE_BUFFER_INITIAL_SIZE);
    if (!line_buffer)
    {
        perror("malloc failed for line buffer");
        return 0;
    }
    size_t line_buffer_capacity = LINE_BUFFER_INITIAL_SIZE;

// --- Preallocate match position storage ---
#define MAX_MATCHES_PER_LINE 2048 // Doubled from original to handle more dense matches
    static match_position_t line_match_positions[MAX_MATCHES_PER_LINE];

    // --- Precompute constant string lengths ---
    // Cache color codes and their lengths for better performance
    const char *color_filename = KREP_COLOR_FILENAME;
    const char *color_reset = KREP_COLOR_RESET;
    const char *color_separator = KREP_COLOR_SEPARATOR;
    const char *color_text = KREP_COLOR_TEXT;
    const char *color_match = KREP_COLOR_MATCH;

    // Precompute lengths to avoid repeated strlen calls
    size_t len_color_reset = color_output_enabled ? strlen(color_reset) : 0;
    size_t len_color_text = color_output_enabled ? strlen(color_text) : 0;
    size_t len_color_match = color_output_enabled ? strlen(color_match) : 0;

    // ========================================================================
    // --- Mode: Only Matching Parts (-o) ---
    // ========================================================================
    if (only_matching)
    {
// Use a larger batch buffer for aggregating output before system calls
#define O_BATCH_BUFFER_SIZE (8 * 1024 * 1024) // 8MB batch buffer (doubled from original)
        static char o_batch_buffer[O_BATCH_BUFFER_SIZE];
        size_t o_batch_pos = 0; // Current position in the batch buffer

        // --- Fast line number tracking ---
        // Precompute newline positions for faster line number calculation
        size_t *newline_positions = NULL;
        size_t num_newlines = 0;
        size_t newline_capacity = 0;

        // Only precompute newline positions if we have more than a threshold number of matches
        if (result->count > 10)
        {
            // Count newlines first to allocate properly
            for (size_t i = 0; i < text_len; i++)
            {
                if (text[i] == '\n')
                    num_newlines++;
            }

            // Allocate array for newline positions
            newline_capacity = num_newlines + 1; // +1 for the implicit newline at the end
            newline_positions = malloc(newline_capacity * sizeof(size_t));

            // Populate the array if allocation succeeded
            if (newline_positions)
            {
                size_t idx = 0;
                for (size_t i = 0; i < text_len; i++)
                {
                    if (text[i] == '\n')
                    {
                        newline_positions[idx++] = i;
                    }
                }
            }
        }

        // Precompute the filename prefix string (including colors if enabled)
        char filename_prefix[PATH_MAX + 64] = ""; // Extra space for colors/separator
        size_t filename_prefix_len = 0;
        if (filename)
        {
            if (color_output_enabled)
            {
                filename_prefix_len = snprintf(filename_prefix, sizeof(filename_prefix), "%s%s%s%s:",
                                               color_filename, filename, color_reset, color_separator);
            }
            else
            {
                filename_prefix_len = snprintf(filename_prefix, sizeof(filename_prefix), "%s:", filename);
            }

            // Safety check on filename_prefix length
            if (filename_prefix_len <= 0 || filename_prefix_len >= sizeof(filename_prefix))
            {
                filename_prefix_len = (sizeof(filename_prefix) > 1) ? sizeof(filename_prefix) - 1 : 0;
                if (filename_prefix_len > 0)
                {
                    filename_prefix[filename_prefix_len] = '\0';
                }
                else
                {
                    filename_prefix_len = 0;
                }
            }
        }

        // --- Process matches in batches for better performance ---
        size_t current_line_number = 1;
        size_t last_scanned_offset = 0;
        size_t last_newline_idx = 0;

        // Pre-allocate a static buffer for line numbers to avoid repeated format calls
        char lineno_buffer[32]; // Large enough for any reasonable line number

        // Iterate through all matches in order
        for (uint64_t i = 0; i < result->count; i++)
        {
            // Check max_count limit before processing each match
            if (max_count != SIZE_MAX && items_printed_count >= max_count)
            {
                break; // Stop processing if limit is reached
            }

            size_t start = result->positions[i].start_offset;
            size_t end = result->positions[i].end_offset;

            // Validation and bounds checking
            if (start >= text_len || start > end)
            {
                continue; // Skip invalid match
            }
            if (end > text_len)
            {
                end = text_len; // Clamp end offset
            }
            size_t len = end - start;

            // --- Optimized Line Number Calculation ---
            // Faster line number calculation using precomputed newline positions when available
            if (newline_positions && num_newlines > 0)
            {
                // Binary search to find the position in the newlines array
                size_t left = 0;
                size_t right = num_newlines - 1;

                // Find the first newline position greater than start
                while (left <= right)
                {
                    size_t mid = left + (right - left) / 2;
                    if (newline_positions[mid] < start)
                    {
                        left = mid + 1;
                    }
                    else
                    {
                        if (mid == 0 || newline_positions[mid - 1] < start)
                        {
                            last_newline_idx = mid;
                            break;
                        }
                        right = mid - 1;
                    }
                }

                // Line number is the index of the first newline after start, plus 1
                // (or the count of newlines before start, plus 1)
                if (last_newline_idx > 0 && newline_positions[last_newline_idx - 1] >= start)
                {
                    last_newline_idx--;
                }
                current_line_number = last_newline_idx + 1;
            }
            else
            {
                // Fallback: Count newlines in the segment from last position to current match
                if (start > last_scanned_offset)
                {
                    const char *scan_ptr = text + last_scanned_offset;
                    const char *end_scan_ptr = text + start;

                    // Fast newline counting with memchr
                    while (scan_ptr < end_scan_ptr)
                    {
                        const void *newline_found = memchr(scan_ptr, '\n', end_scan_ptr - scan_ptr);
                        if (newline_found)
                        {
                            current_line_number++;
                            scan_ptr = (const char *)newline_found + 1;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
            last_scanned_offset = start; // Update for next iteration

            // Format line number into a temporary buffer
            int lineno_len = snprintf(lineno_buffer, sizeof(lineno_buffer), "%zu:", current_line_number);
            if (lineno_len <= 0 || (size_t)lineno_len >= sizeof(lineno_buffer))
            {
                strcpy(lineno_buffer, "ERR:");
                lineno_len = 4;
            }

            // Calculate the total size required in the batch buffer for this entry
            // Note: This is an estimate; actual size might differ slightly if newlines are replaced.
            size_t required_estimate = filename_prefix_len + lineno_len + len + 1; // +1 for newline
            if (color_output_enabled)
            {
                required_estimate += len_color_match + len_color_reset;
            }

            // Flush the batch buffer to stdout if the new entry won't fit (use estimate)
            if (o_batch_pos + required_estimate > O_BATCH_BUFFER_SIZE)
            {
                if (fwrite(o_batch_buffer, 1, o_batch_pos, stdout) != o_batch_pos)
                {
                    perror("Error writing batch buffer to stdout (-o mode)");
                    // Consider how to handle write errors; maybe break or return error count?
                    break;
                }
                o_batch_pos = 0; // Reset batch buffer position
            }

            // --- Efficient append to batch buffer using direct pointer manipulation ---
            char *current_write_ptr = o_batch_buffer + o_batch_pos;
            char *batch_buffer_end = o_batch_buffer + O_BATCH_BUFFER_SIZE; // Boundary check

            // 1. Copy filename prefix (if any)
            if (filename_prefix_len > 0)
            {
                safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, filename_prefix, filename_prefix_len);
            }

            // 2. Copy line number
            safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, lineno_buffer, lineno_len);

            // 3. Start color for match (if enabled)
            if (color_output_enabled)
            {
                safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, color_match, len_color_match);
            }

            // 4. Copy the matched text, replacing internal newlines
            const char *match_ptr = text + start;
            for (size_t k = 0; k < len; ++k)
            {
                char current_char = match_ptr[k];
                if (current_char == '\n')
                {
                    safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, " ", 1);
                }
                else
                {
                    safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, &current_char, 1);
                }
                // Check if buffer became full during character copy
                if (o_batch_pos == O_BATCH_BUFFER_SIZE)
                {
                    break; // Stop copying this match if buffer full
                }
            }

            // Check again if buffer became full during the loop
            if (o_batch_pos == O_BATCH_BUFFER_SIZE)
            {
                // Flush here if needed, or let the outer loop handle it
                continue; // Skip rest of processing for this match
            }

            // 5. End color for match (if enabled)
            if (color_output_enabled)
            {
                safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, color_reset, len_color_reset);
            }

            // 6. Add newline (only if buffer not already full)
            if (o_batch_pos < O_BATCH_BUFFER_SIZE)
            {
                safe_append_to_batch(&current_write_ptr, batch_buffer_end, &o_batch_pos, O_BATCH_BUFFER_SIZE, "\n", 1);
            }

            // Update batch buffer position based on the actual data written
            if (o_batch_pos != O_BATCH_BUFFER_SIZE)
            {
                o_batch_pos = current_write_ptr - o_batch_buffer;
            }
            items_printed_count++;
        }

        // Flush any remaining content in the batch buffer
        if (o_batch_pos > 0)
        {
            fwrite(o_batch_buffer, 1, o_batch_pos, stdout);
        }

        // Free resources
        if (newline_positions)
        {
            free(newline_positions);
        }
    }
    // ========================================================================
    // --- Mode: Full Lines (Default) ---
    // ========================================================================
    else
    {
        size_t last_printed_line_start = SIZE_MAX; // Track the start offset of the last line printed

        // Precompute the filename prefix string (including colors if enabled)
        char filename_prefix[PATH_MAX + 64] = ""; // Extra space for colors/separator
        size_t filename_prefix_len = 0;
        if (filename)
        {
            if (color_output_enabled)
            {
                // Full line starts with filename, separator, then text color
                filename_prefix_len = snprintf(filename_prefix, sizeof(filename_prefix), "%s%s%s%s:%s",
                                               color_filename, filename, color_reset, color_separator, color_text);
            }
            else
            {
                filename_prefix_len = snprintf(filename_prefix, sizeof(filename_prefix), "%s:", filename);
            }

            // Safety check on filename_prefix length
            if (filename_prefix_len <= 0 || filename_prefix_len >= sizeof(filename_prefix))
            {
                filename_prefix_len = (sizeof(filename_prefix) > 1) ? sizeof(filename_prefix) - 1 : 0;
                if (filename_prefix_len > 0)
                {
                    filename_prefix[filename_prefix_len] = '\0';
                }
                else
                {
                    filename_prefix_len = 0;
                }
            }
        }

// --- Create a line batch buffer for full line mode ---
// This buffer aggregates multiple formatted lines before writing to stdout
#define LINE_BATCH_BUFFER_SIZE (8 * 1024 * 1024) // 8MB for batch output
        static char line_batch_buffer[LINE_BATCH_BUFFER_SIZE];
        size_t line_batch_pos = 0;

        // Iterate through matches, processing line by line
        uint64_t i = 0;
        while (i < result->count)
        {
            // Check max_count limit before processing each line
            if (max_count != SIZE_MAX && items_printed_count >= max_count)
            {
                break; // Stop processing if limit is reached
            }

            size_t first_match_start_on_line = result->positions[i].start_offset;

            // Basic validation for the starting match offset
            if (first_match_start_on_line >= text_len)
            {
                i++; // Skip invalid starting match
                continue;
            }

            // Find the start of the line containing this match (optimization: use memrchr if available)
            size_t line_start = find_line_start(text, text_len, first_match_start_on_line); // Use text instead of text_start

            // Check if this line has already been printed in a previous iteration
            if (line_start == last_printed_line_start)
            {
                // Efficiently skip all subsequent matches that start on this *same* line
                // Find the end of the current line first
                size_t current_line_end = find_line_end(text, text_len, line_start); // Use text instead of text_start
                while (i < result->count && result->positions[i].start_offset < current_line_end)
                {
                    i++;
                }
                continue; // Move to the next potential new line
            }

            // Found a new line to process. Find its end boundary.
            size_t line_end = find_line_end(text, text_len, line_start); // Use text instead of text_start

            // --- Collect all matches that fall within this line ---
            size_t line_match_count = 0;
            uint64_t line_match_scan_idx = i; // Start scanning from the current match index

            while (line_match_scan_idx < result->count)
            {
                size_t k_start = result->positions[line_match_scan_idx].start_offset;

                // If the match starts at or after the end of the current line, we're done collecting for this line.
                if (k_start >= line_end)
                {
                    break;
                }

                // Only consider matches that start *on* this line
                if (k_start >= line_start)
                {
                    // Ensure we don't overflow the preallocated line_match_positions buffer
                    if (line_match_count < MAX_MATCHES_PER_LINE)
                    {
                        size_t k_end = result->positions[line_match_scan_idx].end_offset;
                        // Clamp match end to text length for safety
                        if (k_end > text_len)
                            k_end = text_len;

                        // Store the match relative to the start of the text
                        line_match_positions[line_match_count].start_offset = k_start;
                        line_match_positions[line_match_count].end_offset = k_end;
                        line_match_count++;
                    }
                    else
                    {
                        // Log warning if too many matches on one line
                        fprintf(stderr, "Warning: Exceeded MAX_MATCHES_PER_LINE (%d) on line starting at offset %zu in %s\n",
                                MAX_MATCHES_PER_LINE, line_start, filename ? filename : "<stdin>");
                        // Stop collecting matches for this line, but process the ones found so far
                        break;
                    }
                }

                line_match_scan_idx++; // Move to the next potential match
            }

            // --- Pre-calculate required buffer size for the line ---
            size_t line_len = line_end - line_start;
            size_t max_required_size = filename_prefix_len + line_len + 1; // Prefix + content + newline
            if (color_output_enabled)
            {
                // Add space for color codes:
                // - Initial text color (if no prefix)
                // - Match color + text color for each match
                // - Final reset color
                max_required_size += (filename_prefix_len == 0 ? len_color_text : 0) +
                                     (line_match_count * (len_color_match + len_color_text)) +
                                     len_color_reset;
            }

            // --- Ensure line buffer capacity once ---
            if (!ensure_line_buffer_capacity((char **)&line_buffer, &line_buffer_capacity, 0, max_required_size))
            {
                // Handle error: cannot allocate enough buffer space for the line
                fprintf(stderr, "Error: Failed to ensure sufficient buffer capacity (%zu bytes) for line starting at offset %zu in %s\n",
                        max_required_size, line_start, filename ? filename : "<stdin>");
                // Skip processing this line and advance past its matches
                i = line_match_scan_idx;
                continue;
            }

            // --- Format the current line with highlighting ---
            size_t buffer_pos = 0;                 // Current position in line_buffer
            char *current_write_ptr = line_buffer; // Use a direct pointer

            // Add filename prefix if applicable
            if (filename_prefix_len > 0)
            {
                memcpy(current_write_ptr, filename_prefix, filename_prefix_len);
                current_write_ptr += filename_prefix_len;
            }
            else if (color_output_enabled)
            {
                // If no filename, but color is on, start the line with the default text color
                memcpy(current_write_ptr, color_text, len_color_text);
                current_write_ptr += len_color_text;
            }

            // Iterate through the line, copying text segments and highlighted matches
            size_t current_pos_on_line = line_start; // Track position within the original text
            for (size_t k = 0; k < line_match_count; ++k)
            {
                size_t k_start = line_match_positions[k].start_offset;
                size_t k_end = line_match_positions[k].end_offset;

                // Clamp match boundaries strictly to the current line's boundaries
                if (k_start < line_start)
                    k_start = line_start;
                if (k_end > line_end)
                    k_end = line_end;
                if (k_start >= k_end)
                    continue; // Skip zero-length or invalid matches

                // 1. Copy text segment BEFORE the current match
                if (k_start > current_pos_on_line)
                {
                    size_t len_before = k_start - current_pos_on_line;
                    memcpy(current_write_ptr, text + current_pos_on_line, len_before);
                    current_write_ptr += len_before;
                }

                // 2. Copy the highlighted MATCH segment
                size_t match_len = k_end - k_start;
                if (color_output_enabled)
                {
                    memcpy(current_write_ptr, color_match, len_color_match);
                    current_write_ptr += len_color_match;
                }
                memcpy(current_write_ptr, text + k_start, match_len);
                current_write_ptr += match_len;
                if (color_output_enabled)
                {
                    memcpy(current_write_ptr, color_text, len_color_text); // Switch back to text color after match
                    current_write_ptr += len_color_text;
                }

                // Update the position marker within the original text line
                current_pos_on_line = k_end;
            }

            // 3. Copy any remaining text AFTER the last match until the line end
            if (current_pos_on_line < line_end)
            {
                size_t len_after = line_end - current_pos_on_line;
                memcpy(current_write_ptr, text + current_pos_on_line, len_after);
                current_write_ptr += len_after;
            }

            // 4. Add final color reset and newline character
            if (color_output_enabled)
            {
                memcpy(current_write_ptr, color_reset, len_color_reset);
                current_write_ptr += len_color_reset;
            }
            *current_write_ptr = '\n';
            current_write_ptr++;

            // Calculate final buffer position based on pointer arithmetic
            buffer_pos = current_write_ptr - line_buffer;

            // --- Efficient batch output handling ---
            // Check if the newly formatted line fits in the batch buffer
            if (line_batch_pos + buffer_pos > LINE_BATCH_BUFFER_SIZE)
            {
                // Flush the current batch buffer before adding the new line
                if (fwrite(line_batch_buffer, 1, line_batch_pos, stdout) != line_batch_pos)
                {
                    perror("Error writing line batch buffer to stdout");
                    // Consider how to handle this error; maybe stop processing?
                }
                line_batch_pos = 0; // Reset batch buffer position
            }

            // Copy formatted line to batch buffer (only if it fits after potential flush)
            // This check prevents buffer overflow if a single line exceeds LINE_BATCH_BUFFER_SIZE
            if (buffer_pos <= LINE_BATCH_BUFFER_SIZE)
            {
                memcpy(line_batch_buffer + line_batch_pos, line_buffer, buffer_pos);
                line_batch_pos += buffer_pos;
            }
            else
            {
                // If a single line is too large, write it directly (or handle error)
                fprintf(stderr, "Warning: Single line exceeds batch buffer size (%zu > %d). Writing directly.\n",
                        buffer_pos, LINE_BATCH_BUFFER_SIZE);
                if (fwrite(line_buffer, 1, buffer_pos, stdout) != buffer_pos)
                {
                    perror("Error writing oversized line directly to stdout");
                }
            }

            // Update tracking variables
            items_printed_count++;                // Increment after successfully printing/batching a line
            last_printed_line_start = line_start; // Mark this line as printed

            // Advance the main loop index 'i' past all matches processed for this line
            i = line_match_scan_idx;
            continue; // Continue to the next potential line
        }

        // Flush any remaining content in the line batch buffer
        if (line_batch_pos > 0)
        {
            if (fwrite(line_batch_buffer, 1, line_batch_pos, stdout) != line_batch_pos)
            {
                perror("Error writing final line batch buffer to stdout");
            }
        }
    }

    // --- Cleanup ---
    fflush(stdout);
    free(line_buffer);

    return items_printed_count;
}

// --- Utility Functions ---

// Helper function to ensure a buffer has enough capacity, reallocating if needed.
// Returns true on success, false on allocation failure.
static bool ensure_line_buffer_capacity(char **buffer_ptr, size_t *capacity_ptr, size_t current_pos, size_t needed)
{
    if (current_pos + needed > *capacity_ptr)
    {
        size_t new_capacity = *capacity_ptr;
        if (new_capacity == 0)
        {
            new_capacity = 1024; // Start with a reasonable size
        }
        // Double the capacity until it's large enough
        while (new_capacity < current_pos + needed)
        {
            // Check for potential overflow before doubling
            if (new_capacity > SIZE_MAX / 2)
            {
                // If doubling would overflow, try setting to the exact needed size + some buffer
                // This is a last resort and might still fail if needed is too large
                new_capacity = current_pos + needed + 1024;
                if (new_capacity < current_pos + needed)
                { // Check overflow again
                    fprintf(stderr, "Error: Cannot allocate required buffer capacity (overflow).\n");
                    return false;
                }
                break; // Exit loop after setting to required size
            }
            new_capacity *= 2;
        }

        char *new_buffer = realloc(*buffer_ptr, new_capacity);
        if (!new_buffer)
        {
            perror("realloc failed for buffer");
            return false;
        }
        *buffer_ptr = new_buffer;
        *capacity_ptr = new_capacity;
    }
    return true;
}

// Get monotonic time
double get_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        perror("Cannot get monotonic time");
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Print usage information
void print_usage(const char *program_name)
{
    printf("krep v%s - A high-performance string search utility\n\n", VERSION);
    printf("Usage: %s [OPTIONS] PATTERN [FILE | DIRECTORY]\n", program_name);
    printf("   or: %s [OPTIONS] -e PATTERN [-e PATTERN...] [FILE | DIRECTORY]\n", program_name);
    printf("   or: %s [OPTIONS] -f FILE [FILE | DIRECTORY]\n", program_name);
    printf("   or: %s [OPTIONS] -s PATTERN STRING_TO_SEARCH\n", program_name);
    printf("   or: %s [OPTIONS] PATTERN < FILE\n", program_name);
    printf("   or: cat FILE | %s [OPTIONS] PATTERN\n\n", program_name);
    printf("OPTIONS:\n");
    printf("  -i             Perform case-insensitive matching.\n");
    printf("  -c             Count matching lines. Only a count of lines is printed.\n");
    printf("  -o             Only matching. Print only the matched parts of lines, one per line.\n");
    printf("  -e PATTERN     Specify pattern. Can be used multiple times (treated as OR for literal, combined for regex).\n");
    printf("  -f FILE        Read patterns from FILE, one per line.\n");
    printf("  -E             Interpret PATTERN(s) as POSIX Extended Regular Expression(s).\n");
    printf("                 If multiple -e used with -E, they are combined with '|'.\n");
    printf("  -F             Interpret PATTERN(s) as fixed strings (literal). Default if not -E.\n");
    printf("  -r             Search directories recursively. Skips binary files and common dirs.\n");
    printf("  -t NUM         Use NUM threads for file search (default: auto-detect cores).\n");
    printf("  -s             Search in STRING_TO_SEARCH instead of FILE or DIRECTORY.\n");
    printf("  --color[=WHEN] Control color output ('always', 'never', 'auto'). Default: 'auto'.\n");
    printf("  --no-simd      Explicitly disable SIMD acceleration.\n");
    printf("  -v             Show version information and exit.\n");
    printf("  -h, --help     Show this help message and exit.\n");
    printf("  -m NUM         Stop reading a file after NUM matching lines.\n");
    printf("  -w             Select only matches that form whole words.\n\n");
    printf("EXIT STATUS:\n");
    printf("  0 if matches were found,\n");
    printf("  1 if no matches were found,\n");
    printf("  2 if an error occurred.\n\n");
    printf("EXAMPLES:\n");
    printf("  %s \"search term\" input.log\n", program_name);
    printf("  %s -i -c ERROR large_log.txt\n", program_name);
    printf("  %s -t 8 -o '[0-9]+' data.log | sort | uniq -c\n", program_name);
    printf("  %s -E \"^[Ee]rror: .*failed\" system.log\n", program_name);
    printf("  %s -r \"MyClass\" /path/to/project\n", program_name);
    printf("  %s -e Error -e Warning app.log\n", program_name); // Find lines with Error OR Warning
}

// Helper for case-insensitive comparison using the lookup table
inline bool memory_equals_case_insensitive(const unsigned char *s1, const unsigned char *s2, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (lower_table[s1[i]] != lower_table[s2[i]])
        {
            return false;
        }
    }
    return true;
}

// --- Boyer-Moore-Horspool Algorithm with Turbo Shift ---

// Prepare the bad character table for BMH
void prepare_bad_char_table(const unsigned char *pattern, size_t pattern_len, int *bad_char_table, bool case_sensitive)
{
    // Initialize all shifts to pattern length
    for (int i = 0; i < 256; i++)
    {
        bad_char_table[i] = (int)pattern_len;
    }
    // Calculate shifts for characters actually in the pattern (excluding the last character)
    // The shift is the distance from the end of the pattern.
    for (size_t i = 0; i < pattern_len - 1; i++)
    {
        unsigned char c = pattern[i];
        int shift = (int)(pattern_len - 1 - i);
        if (!case_sensitive)
        {
            unsigned char lc = lower_table[c];
            // Set the minimum shift for this character (rightmost occurrence determines shift)
            if (shift < bad_char_table[lc])
            {
                bad_char_table[lc] = shift;
            }
            // Also set for the uppercase equivalent if different
            unsigned char uc = toupper(c); // Use standard toupper for the other case
            if (uc != lc)
            {
                if (shift < bad_char_table[uc])
                {
                    bad_char_table[uc] = shift;
                }
            }
        }
        else
        {
            // Set the minimum shift
            if (shift < bad_char_table[c])
            {
                bad_char_table[c] = shift;
            }
        }
    }
}

// Prepare good suffix table for Boyer-Moore (turbo shift enhancement)
static void prepare_good_suffix_table(const unsigned char *pattern, size_t pattern_len, 
                                       int *good_suffix_table, int *suffix_table)
{
    size_t m = pattern_len;
    
    // Compute suffix table
    suffix_table[m - 1] = m;
    int g = m - 1;
    int f = 0;
    
    for (int i = (int)m - 2; i >= 0; --i)
    {
        if (i > g && suffix_table[i + m - 1 - f] < i - g)
        {
            suffix_table[i] = suffix_table[i + m - 1 - f];
        }
        else
        {
            if (i < g)
                g = i;
            f = i;
            while (g >= 0 && pattern[g] == pattern[g + m - 1 - f])
                --g;
            suffix_table[i] = f - g;
        }
    }
    
    // Compute good suffix shifts
    for (size_t i = 0; i < m; i++)
    {
        good_suffix_table[i] = m;
    }
    
    int j = 0;
    for (int i = (int)m - 1; i >= 0; --i)
    {
        if (suffix_table[i] == i + 1)
        {
            for (; j < (int)m - 1 - i; ++j)
            {
                if (good_suffix_table[j] == (int)m)
                {
                    good_suffix_table[j] = (int)m - 1 - i;
                }
            }
        }
    }
    
    for (size_t i = 0; i <= m - 2; i++)
    {
        good_suffix_table[m - 1 - suffix_table[i]] = (int)m - 1 - i;
    }
}

// Adds positions to 'result' if params->track_positions is true.
// Enhanced with turbo shift and prefetching for maximum performance
HOT_FUNCTION
uint64_t boyer_moore_search(const search_params_t *params,
                            const char *text_start,
                            size_t text_len,
                            match_result_t *result) // For position tracking (can be NULL)
{
    // --- Add max_count == 0 check ---
    if (UNLIKELY(params->max_count == 0 && (params->count_lines_mode || params->track_positions)))
        return 0;
    // --- End add ---

    const unsigned char *utext_start = (const unsigned char *)text_start;
    const unsigned char *search_pattern = (const unsigned char *)params->pattern;
    size_t pattern_len = params->pattern_len;
    bool case_sensitive = params->case_sensitive;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count;

    if (UNLIKELY(pattern_len == 0 || text_len < pattern_len))
        return 0;

    // Prepare bad character table
    int bad_char_table[256];
    prepare_bad_char_table(search_pattern, pattern_len, bad_char_table, case_sensitive);
    
    // Prepare good suffix table for turbo shift (only for longer patterns)
    int *good_suffix_table = NULL;
    int *suffix_table = NULL;
    bool use_turbo = (pattern_len >= 4 && pattern_len <= 256);
    
    if (use_turbo)
    {
        good_suffix_table = malloc(pattern_len * sizeof(int));
        suffix_table = malloc(pattern_len * sizeof(int));
        if (good_suffix_table && suffix_table)
        {
            prepare_good_suffix_table(search_pattern, pattern_len, good_suffix_table, suffix_table);
        }
        else
        {
            // Fallback if allocation fails
            use_turbo = false;
            free(good_suffix_table);
            free(suffix_table);
            good_suffix_table = NULL;
            suffix_table = NULL;
        }
    }

    uint64_t current_count = 0;
    size_t last_counted_line_start = SIZE_MAX;
    size_t i = 0;
    size_t search_limit = text_len - pattern_len + 1;

    // Hoist pattern's last char once
    unsigned char pc_last = search_pattern[pattern_len - 1];
    unsigned char pc_last_lower = case_sensitive ? pc_last : lower_table[pc_last];

    // Turbo shift variables
    size_t turbo_shift = 0;
    size_t turbo_len = 0;

    while (i < search_limit)
    {
        // Prefetch ahead for better cache utilization
        if (LIKELY(i + PREFETCH_DISTANCE < text_len))
            __builtin_prefetch(utext_start + i + PREFETCH_DISTANCE, 0, 0);

        unsigned char tc_last = utext_start[i + pattern_len - 1];
        unsigned char tc_last_cmp = case_sensitive ? tc_last : lower_table[tc_last];

        bool last_char_match = (tc_last_cmp == pc_last_lower);

        if (last_char_match)
        {
            bool full_match = true;
            size_t j = pattern_len - 1;
            
            // Compare from right to left
            if (pattern_len > 1)
            {
                if (case_sensitive)
                {
                    // Optimized comparison with early exit
                    for (j = pattern_len - 1; j > 0; --j)
                    {
                        if (utext_start[i + j - 1] != search_pattern[j - 1])
                        {
                            full_match = false;
                            break;
                        }
                    }
                }
                else
                {
                    full_match = memory_equals_case_insensitive(utext_start + i, search_pattern, pattern_len - 1);
                }
            }

            if (full_match)
            {
                // Whole word check
                if (params->whole_word && !is_whole_word_match(text_start, text_len, i, i + pattern_len))
                {
                    unsigned char bad = tc_last;
                    int shift_val = bad_char_table[bad];
                    i += shift_val;
                    turbo_shift = 0;
                    continue;
                }
                
                bool count_incremented_this_match = false;
                if (count_lines_mode)
                {
                    size_t line_start = find_line_start(text_start, text_len, i);
                    if (line_start != last_counted_line_start)
                    {
                        current_count++;
                        last_counted_line_start = line_start;
                        count_incremented_this_match = true;
                    }
                }
                else
                {
                    current_count++;
                    count_incremented_this_match = true;
                    if (track_positions && result && current_count <= max_count)
                    {
                        if (!match_result_add(result, i, i + pattern_len))
                        {
                            // Warning: allocation failed, ignore location
                        }
                    }
                }

                if (count_incremented_this_match && current_count >= max_count)
                    break;

                // After match: advance by pattern length (non-overlapping matches)
                if (only_matching && !params->count_lines_mode)
                    i += pattern_len;
                else
                {
                    int shift_val = bad_char_table[tc_last];
                    i += (shift_val > 1) ? shift_val : 1;
                }
                turbo_shift = 0;
                turbo_len = 0;
                continue;
            }
            else
            {
                // Mismatch occurred - compute shift using both heuristics
                int bc_shift = bad_char_table[tc_last];
                int gs_shift = 1;
                
                if (use_turbo && good_suffix_table)
                {
                    gs_shift = good_suffix_table[j];
                    
                    // Turbo-BM optimization
                    if (turbo_len >= j && turbo_shift != (size_t)gs_shift)
                    {
                        size_t shift = (turbo_len > pattern_len - j) ? 
                                        turbo_len - pattern_len + j + 1 : 1;
                        if (shift > (size_t)gs_shift)
                            gs_shift = shift;
                    }
                }
                
                int shift_val = (bc_shift > gs_shift) ? bc_shift : gs_shift;
                
                // Save turbo state
                turbo_len = (bc_shift > gs_shift) ? 0 : pattern_len - j;
                turbo_shift = shift_val;
                
                i += shift_val;
                continue;
            }
        }

        // No last char match - use bad character shift
        int shift_val = bad_char_table[tc_last];
        turbo_shift = 0;
        turbo_len = 0;
        i += shift_val;
    }

    // Cleanup
    if (good_suffix_table) free(good_suffix_table);
    if (suffix_table) free(suffix_table);

    return current_count;
}

// --- Regex Search ---

uint64_t regex_search(const search_params_t *params,
                      const char *text_start,
                      size_t text_len,
                      match_result_t *result)
{
    // 1) If limit is zero, no matches.
    if (params->max_count == 0 && (params->count_lines_mode || params->track_positions)) // Check both modes
        return 0;

    // Must have a compiled regex.
    if (!params->compiled_regex)
        return 0;

    // Special‐case empty haystack: allow zero‐length match like ^$
    if (text_len == 0)
    {
        regmatch_t m;
        if (regexec(params->compiled_regex, "", 1, &m, 0) == 0)
        {
            // count‐lines vs track_positions
            if (params->count_lines_mode)
                return 1;
            if (params->track_positions && result)
                match_result_add(result, 0, 0);
            return 1;
        }
        return 0;
    }

    const regex_t *regex = params->compiled_regex;
    regmatch_t pmatch[1];
    int base_eflags = REG_STARTEND | REG_NEWLINE | (params->case_sensitive ? 0 : REG_ICASE); // REG_NEWLINE is already part of base_eflags through compilation flags
    const char *cur = text_start;
    size_t rem = text_len;
    size_t last_line = SIZE_MAX;
    uint64_t count = 0;
    size_t max_count = params->max_count; // Get max_count

    while (rem > 0 || (rem == 0 && cur == text_start)) // Allow one check for empty string match
    {
        // Ensure we don't search past the end if rem becomes 0 mid-loop
        pmatch[0].rm_so = 0;
        pmatch[0].rm_eo = rem; // Search up to the remaining length
        // REG_NOTBOL is set if we are not at the absolute start of the original text
        int eflags = base_eflags | ((cur == text_start) ? 0 : REG_NOTBOL);

        int rc = regexec(regex, cur, 1, pmatch, eflags);

        if (rc != 0)
        {
            if (rc == REG_NOMATCH)
            {
                break; // No more matches found
            }
            else
            {
                // Handle regex execution error
                char ebuf[256];
                regerror(rc, regex, ebuf, sizeof(ebuf));
                fprintf(stderr, "krep: Regex execution error: %s\n", ebuf);
                // Consider returning an error indicator or specific count
                return count; // Return count found so far on error
            }
        }

        // Check for -1 offsets which indicate failure (shouldn't happen if rc == 0)
        if (pmatch[0].rm_so == -1 || pmatch[0].rm_eo == -1)
        {
            fprintf(stderr, "krep: Warning: regexec returned success but invalid offsets.\n");
            break; // Treat as no match / error
        }

        size_t so = pmatch[0].rm_so; // Offset relative to 'cur'
        size_t eo = pmatch[0].rm_eo; // Offset relative to 'cur'

        // Ensure eo >= so (sanity check)
        if (eo < so)
        {
            fprintf(stderr, "krep: Warning: regexec returned eo < so.\n");
            // Advance past this point to avoid infinite loop
            const char *next_cur = cur + so + 1;
            if (next_cur > text_start + text_len)
            {
                cur = text_start + text_len;
            }
            else
            {
                cur = next_cur;
            }
            rem = (text_start + text_len) - cur;
            continue;
        }

        size_t start = (cur - text_start) + so; // Absolute start offset
        size_t end = (cur - text_start) + eo;   // Absolute end offset

        // Whole word check
        if (params->whole_word && !is_whole_word_match(text_start, text_len, start, end))
        {
            // If whole word check fails, we need to advance past the start of this failed match
            // Advance 'cur' by the start offset of the failed match within 'cur' + 1
            const char *next_cur = cur + so + 1;
            if (next_cur > text_start + text_len)
            {
                cur = text_start + text_len;
            }
            else
            {
                cur = next_cur;
            }
            rem = (text_start + text_len) - cur;
            continue;
        }

        if (params->count_lines_mode)
        {
            size_t line_start_offset = find_line_start(text_start, text_len, start);
            if (line_start_offset != last_line)
            {
                count++;
                last_line = line_start_offset;
            }
        }
        else
        {
            count++;
            if (params->track_positions && result)
            {
                match_result_add(result, start, end);
            }
        }

        // Check max_count limit
        if (count >= max_count)
            break;

        // Advance cur to continue searching from the end of the current match.
        // If the match was zero-length, advance by one character from the start of the match
        // to prevent infinite loops and ensure progress.
        size_t advance_by_in_slice = eo; // End offset of match within the current slice `cur`
        if (so == eo)
        {                                 // Zero-length match
            advance_by_in_slice = so + 1; // Advance by 1 from the start of the zero-length match
        }

        // Ensure that cur always advances if a match is found and we are not at the end of text.
        // This is particularly important if advance_by_in_slice could somehow be 0 when so != eo (should not happen).
        // The (so == eo) case already ensures advance_by_in_slice is at least so + 1.
        // If so < eo, then advance_by_in_slice = eo > so, so cur will advance.

        const char *next_search_start = cur + advance_by_in_slice;

        if (next_search_start > text_start + text_len)
        {
            cur = text_start + text_len; // Move to the very end
        }
        else if (next_search_start <= cur && text_len > 0 && (cur < text_start + text_len))
        {
            // This case should ideally not be hit if so <= eo and zero-length matches advance by at least 1.
            // Force advancement by at least one character from current `cur` if stuck.
            // This might happen if `so` and `eo` are both 0 and `cur` is not advanced.
            // The `so + 1` for zero-length matches should prevent this.
            // As a safeguard:
            cur = cur + 1;
        }
        else
        {
            cur = next_search_start;
        }

        if (cur > text_start + text_len)
        { // Should be caught by prior check, but defensive
            cur = text_start + text_len;
        }
        rem = (text_start + text_len) - cur;

    } // end while

    return count;
}

// --- Knuth-Morris-Pratt (KMP) Algorithm ---

// Compute the Longest Proper Prefix which is also Suffix (LPS) array
// lps[i] = length of the longest proper prefix of pattern[0..i] which is also a suffix of pattern[0..i]
static void compute_lps_array(const unsigned char *pattern, size_t pattern_len, int *lps, bool case_sensitive)
{
    size_t length = 0; // length of the previous longest prefix suffix
    lps[0] = 0;        // lps[0] is always 0
    size_t i = 1;

    // Calculate lps[i] for i = 1 to pattern_len-1
    while (i < pattern_len)
    {
        // Compare pattern[i] with the character after the current prefix suffix (pattern[length])
        unsigned char char_i = case_sensitive ? pattern[i] : lower_table[pattern[i]];
        unsigned char char_len = case_sensitive ? pattern[length] : lower_table[pattern[length]];

        if (char_i == char_len)
        {
            // Match: extend the current prefix suffix length
            length++;
            lps[i] = length;
            i++;
        }
        else
        {
            // Mismatch
            if (length != 0)
            {
                // Fall back using the LPS value of the previous character in the prefix suffix
                // This allows us to reuse the previously computed information.
                length = lps[length - 1];
                // Do not increment i here, retry comparison with the new 'length'
            }
            else
            {
                // If length is 0, there's no prefix suffix ending here
                lps[i] = 0;
                i++; // Move to the next character
            }
        }
    }
}

// KMP search function (Corrected advancement for overlaps)
// Returns line count (-c) or match count (other modes).
// Adds positions to 'result' if params->track_positions is true.
uint64_t kmp_search(const search_params_t *params,
                    const char *text_start,
                    size_t text_len,
                    match_result_t *result) // For position tracking (can be NULL)
{
    // --- Add max_count == 0 check ---
    if (params->max_count == 0)
        return 0;
    // --- End add ---

    uint64_t current_count = 0; // Use local counter for limit check
    const unsigned char *search_pattern = (const unsigned char *)params->pattern;
    size_t pattern_len = params->pattern_len;
    bool case_sensitive = params->case_sensitive;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count; // Get max_count

    if (pattern_len == 0 || text_len < pattern_len)
        return 0;

    // Precompute LPS array
    int *lps = malloc(pattern_len * sizeof(int));
    if (!lps)
    {
        perror("malloc failed for KMP LPS array");
        return 0; // Indicate error or handle differently
    }
    compute_lps_array(search_pattern, pattern_len, lps, case_sensitive);

    size_t i = 0; // index for text_start[]
    size_t j = 0; // index for search_pattern[]
    const unsigned char *utext_start = (const unsigned char *)text_start;
    size_t last_counted_line_start = SIZE_MAX; // For -c mode tracking

    while (i < text_len)
    {
        // Compare current characters (case-sensitive or insensitive)
        unsigned char char_text = case_sensitive ? utext_start[i] : lower_table[utext_start[i]];
        unsigned char char_patt = case_sensitive ? search_pattern[j] : lower_table[search_pattern[j]];

        if (char_patt == char_text)
        {
            // Match: advance both text and pattern indices
            i++;
            j++;
        }

        // If pattern index 'j' reaches pattern_len, a full match is found
        if (j == pattern_len)
        {
            // Match found ending at index i-1, starting at i - j
            size_t match_start_index = i - j;

            // --- Match Found ---
            // Whole word check
            if (params->whole_word && !is_whole_word_match(text_start, text_len, match_start_index, match_start_index + pattern_len))
            {
                j = 0;
                continue;
            }

            if (count_lines_mode) // -c mode
            {
                size_t line_start = find_line_start(text_start, text_len, match_start_index);
                if (line_start != last_counted_line_start)
                {
                    // --- Check max_count BEFORE incrementing ---
                    if (max_count != SIZE_MAX && current_count >= max_count)
                    {
                        break; // Limit reached
                    }
                    // --- End check ---

                    current_count++; // Increment line count
                    last_counted_line_start = line_start;

                    // Skip to end of current line (optimization for -c mode)
                    size_t line_end = find_line_end(text_start, text_len, line_start);
                    i = (line_end < text_len) ? line_end + 1 : text_len;
                    j = 0;    // Reset pattern index
                    continue; // Continue outer loop from the potentially advanced 'i'
                }
                // If match is on an already counted line, just update j and continue
                j = 0;
            }
            else // Not -c mode (default, -o, or -co)
            {
                // --- Check max_count BEFORE incrementing ---
                if (max_count != SIZE_MAX && current_count >= max_count)
                {
                    if (track_positions && result) // Add final match
                    {
                        match_result_add(result, match_start_index, match_start_index + pattern_len);
                    }
                    break; // Limit reached
                }
                // --- End check ---

                current_count++; // Increment match count

                if (track_positions && result) // If tracking positions (default or -o)
                {
                    // Add the match position without deduplication
                    if (!match_result_add(result, match_start_index, match_start_index + pattern_len))
                    {
                        fprintf(stderr, "Warning: Failed to add match position (KMP).\n");
                    }
                }

                // For pattern "11", we need to be more selective - advance by exactly the pattern
                // length to match ripgrep's behavior (prevents finding "11" in "1111" at positions 0-1, 1-2, 2-3)
                // The key insight is that for -o mode, we need non-overlapping matches
                i = match_start_index + pattern_len; // This is the critical line - always advance by full pattern length
                j = 0;                               // Reset pattern index
            }
        }
        // Mismatch after j matches (or j == 0)
        else if (i < text_len && char_patt != char_text)
        {
            // If mismatch occurred after some initial match (j > 0),
            // use the LPS array to shift the pattern appropriately.
            // We don't need to compare characters pattern[0..lps[j-1]-1] again,
            // as they will match anyway. Don't advance 'i'.
            if (j != 0)
            {
                j = lps[j - 1];
            }
            else
            {
                // If mismatch occurred at the first character (j == 0),
                // simply advance the text index 'i'.
                i++;
            }
        }
    } // end while

    free(lps);            // Free the LPS array
    return current_count; // Return line count or match count
}

// --- Search Orchestration ---

search_func_t select_search_algorithm(const search_params_t *params)
{
    // Use regex search if requested
    if (params->use_regex)
    {
        return regex_search;
    }

    // Use Aho-Corasick for multiple literal patterns
    if (params->num_patterns > 1 && !params->use_regex)
    {
        return aho_corasick_search;
    }

    // --- Single Literal Pattern ---

    // Check if SIMD can be used:
    bool can_use_simd = !force_no_simd && SIMD_MAX_PATTERN_LEN > 0 && params->pattern_len <= SIMD_MAX_PATTERN_LEN;

    // First, handle very short patterns (1-3 characters) specially
    const size_t SHORT_PATTERN_THRESH = 4; // Patterns of length 1-3 use specialized algorithms

    if (params->pattern_len == 1)
    {
        // For single-character patterns, use ultra-fast memchr approach
        return memchr_search;
    }
    else if (params->pattern_len < SHORT_PATTERN_THRESH)
    {
        // For 2-3 character patterns, use our specialized short pattern search
        // SIMD might still be better for case-sensitive search on supported platforms
        if (can_use_simd && params->case_sensitive)
        {
// Use SIMD for short case-sensitive patterns if available
#if KREP_USE_AVX512
            return simd_avx512_search;
#elif KREP_USE_AVX2
            return simd_avx2_search;
#elif KREP_USE_SSE42
            return simd_sse42_search;
#elif KREP_USE_NEON
            return neon_search;
#else
            return memchr_short_search;
#endif
        }
        else
        {
            // Use our specialized function for short patterns (handles case-insensitive well)
            return memchr_short_search;
        }
    }

    // For patterns 4 characters or longer, follow existing logic
    if (can_use_simd)
    {
#if KREP_USE_AVX512
        // AVX-512 supports patterns up to 64 bytes (case-sensitive only)
        if (params->pattern_len <= 64 && params->case_sensitive)
            return simd_avx512_search;
#endif
#if KREP_USE_AVX2
        // AVX2 supports case-insensitive up to 32 bytes
        if (params->pattern_len <= 32)
            return simd_avx2_search;
#endif
#if KREP_USE_SSE42
        // SSE4.2 only supports case-sensitive up to 16 bytes
        if (params->pattern_len <= 16 && params->case_sensitive)
            return simd_sse42_search;
#endif
#if KREP_USE_NEON
        // NEON supports case-sensitive for any length (using first-byte filter)
        if (params->case_sensitive)
            return neon_search;
#endif
    }

    // Fallback to scalar algorithms for longer patterns
    const size_t KMP_THRESH = 8; // Increased threshold - KMP becomes more efficient for certain patterns

    if (params->pattern_len < KMP_THRESH && is_repetitive_pattern(params->pattern, params->pattern_len))
    {
        return kmp_search; // KMP is better for repetitive patterns
    }
    else
    {
        return boyer_moore_search; // Generally best for most pattern types
    }
}

// Helper function to detect repetitive patterns where KMP might perform better
static bool is_repetitive_pattern(const char *pattern, size_t pattern_len)
{
    if (pattern_len < 3)
        return false;

    // Look for repeating characters or short sequences
    size_t repeats = 0;
    char prev = pattern[0];

    for (size_t i = 1; i < pattern_len; i++)
    {
        if (pattern[i] == prev)
        {
            repeats++;
            if (repeats >= pattern_len / 2)
                return true;
        }
        else
        {
            repeats = 0;
            prev = pattern[i];
        }
    }

    // Check for short repeating sequences (ab, aba, abab, etc.)
    for (size_t seq_len = 2; seq_len <= pattern_len / 2; seq_len++)
    {
        bool is_repetitive = true;
        for (size_t i = seq_len; i < pattern_len; i++)
        {
            if (pattern[i] != pattern[i % seq_len])
            {
                is_repetitive = false;
                break;
            }
        }
        if (is_repetitive)
            return true;
    }

    return false;
}

// --- Threading Logic ---

// Function executed by each search thread (handles single or multiple patterns)
void *search_chunk_thread(void *arg)
{
    thread_data_t *data = (thread_data_t *)arg;
    match_result_t *local_result = NULL; // Local results if tracking positions
    uint64_t count_result = 0;           // Line or match count

    // Allocate local result storage if tracking positions
    if (data->params->track_positions)
    {
        // Estimate initial capacity based on chunk length
        uint64_t initial_cap = (data->chunk_len / 1000 > 100) ? data->chunk_len / 1000 : 100;
        local_result = match_result_init(initial_cap);
        if (!local_result)
        {
            fprintf(stderr, "krep: Thread %d: Failed to allocate local match results.\n", data->thread_id);
            data->error_flag = true;
            return NULL; // Signal error
        }
        data->local_result = local_result; // Store pointer for the main thread
    }

    // Select and run the search algorithm on the assigned chunk
    // Pass local_result (can be NULL if not tracking positions)
    // For multiple patterns, select_search_algorithm should return aho_corasick_search
    search_func_t search_algo = data->search_algo;
    if (!search_algo)
    {
        search_algo = select_search_algorithm(data->params);
        data->search_algo = search_algo;
    }

    count_result = search_algo(data->params,
                               data->chunk_start,
                               data->chunk_len,
                               local_result); // Pass NULL if track_positions is false

    // Store the count (lines or matches) found by this thread
    data->count_result = count_result;

    return NULL; // Success
}

// --- Public API Implementations ---

// Add get_algorithm_name implementation here before search_string function
const char *get_algorithm_name(search_func_t func)
{
    if (func == boyer_moore_search)
        return "Boyer-Moore-Turbo";
    else if (func == kmp_search)
        return "Knuth-Morris-Pratt";
    else if (func == regex_search)
        return "Regex";
    else if (func == aho_corasick_search)
        return "Aho-Corasick";
    else if (func == memchr_search)
        return "memchr";
    else if (func == memchr_short_search)
        return "memchr-short";
#if KREP_USE_SSE42
    else if (func == simd_sse42_search)
        return "SSE4.2";
#endif
#if KREP_USE_AVX2
    else if (func == simd_avx2_search)
        return "AVX2";
#endif
#if KREP_USE_AVX512
    else if (func == simd_avx512_search)
        return "AVX-512";
#endif
#if KREP_USE_NEON
    else if (func == neon_search)
        return "NEON";
#endif
    else
        return "Unknown";
}

// Search a string (remains single-threaded)
int search_string(const search_params_t *params, const char *text)
{
    // Initialize resources to NULL/0 for safe cleanup
    size_t text_len = 0;
    uint64_t final_count = 0;
    match_result_t *matches = NULL;
    int result_code = 1; // Default: no match
    regex_t compiled_regex_local;
    char *combined_regex_pattern = NULL;
    bool regex_compiled = false;
    search_params_t current_params = *params; // Make a mutable copy
    ac_trie_t *local_ac_trie = NULL;          // Pointer for locally built trie

    // --- Validation ---
    if (current_params.num_patterns == 0)
    {
        fprintf(stderr, "Error: No pattern specified.\n");
        return 2;
    }

    if (!text)
    {
        fprintf(stderr, "Error: NULL text in search_string.\n");
        return 2;
    }

    text_len = strlen(text);

    // Validate pattern length for literal search
    if (!current_params.use_regex)
    {
        for (size_t i = 0; i < current_params.num_patterns; ++i)
        {
            // Allow single empty pattern
            if (current_params.pattern_lens[i] == 0)
            {
                if (current_params.num_patterns > 1)
                {
                    fprintf(stderr, "Error: Empty pattern provided for literal search with multiple patterns.\n");
                    return 2;
                }
                // Single empty pattern is OK, Aho-Corasick handles this
            }
            else if (current_params.pattern_lens[i] > MAX_PATTERN_LENGTH)
            {
                fprintf(stderr, "Error: Pattern '%s' too long (max %d).\n",
                        current_params.patterns[i], MAX_PATTERN_LENGTH);
                return 2;
            }
        }
    }

    // --- Resource Allocation ---

    // Allocate results structure if tracking positions
    if (current_params.track_positions)
    {
        // Start with a reasonable capacity based on text length
        uint64_t initial_capacity = text_len > 10000 ? 1000 : 16;
        matches = match_result_init(initial_capacity);
        if (!matches)
        {
            fprintf(stderr, "Error: Cannot allocate memory for match results.\n");
            return 2;
        }
    }

    // --- Build Aho-Corasick Trie (if needed) ---
    bool needs_ac_trie = (params->num_patterns > 1 && !params->use_regex);
    if (needs_ac_trie)
    {
        local_ac_trie = ac_trie_build(&current_params);
        if (!local_ac_trie)
        {
            fprintf(stderr, "krep: Error building Aho-Corasick trie.\n");
            result_code = 2;
            goto cleanup; // Use goto for consistent cleanup
        }
        current_params.ac_trie = local_ac_trie; // Assign to the mutable params copy
    }

    // Compile regex if needed
    if (current_params.use_regex)
    {
        const char *regex_to_compile = NULL;

        // Handle multiple patterns (combine with OR)
        if (current_params.num_patterns > 1)
        {
            // Calculate required buffer size
            size_t total_len = 0;
            for (size_t i = 0; i < current_params.num_patterns; ++i)
            {
                // Add 6 for wrapping with (\b...\b)
                total_len += current_params.pattern_lens[i] + (current_params.whole_word ? 6 : 2) + 1; // () or (\b...\b) + |
            }

            // Allocate and build combined pattern
            combined_regex_pattern = malloc(total_len + 1);
            if (!combined_regex_pattern)
            {
                fprintf(stderr, "krep: Failed to allocate memory for combined regex.\n");
                goto cleanup;
            }

            // Construct the combined pattern string
            char *ptr = combined_regex_pattern;
            for (size_t i = 0; i < current_params.num_patterns; ++i)
            {
                if (current_params.whole_word)
                    ptr += sprintf(ptr, "(\\b%s\\b)", current_params.patterns[i]);
                else
                    ptr += sprintf(ptr, "(%s)", current_params.patterns[i]);
                if (i < current_params.num_patterns - 1)
                {
                    ptr += sprintf(ptr, "|");
                }
            }
            *ptr = '\0';
            regex_to_compile = combined_regex_pattern;
        }
        else if (current_params.num_patterns == 1)
        {
            if (current_params.whole_word)
            {
                size_t len = strlen(current_params.patterns[0]);
                char *tmp = malloc(len + 7); // (\b) + pattern + (\b) + null
                if (!tmp)
                {
                    fprintf(stderr, "krep: Failed to allocate memory for regex pattern.\n");
                    return 2;
                }
                sprintf(tmp, "\\b%s\\b", current_params.patterns[0]);
                regex_to_compile = tmp;
                free(combined_regex_pattern); // In case it was set
                combined_regex_pattern = tmp; // So it gets freed later
            }
            else
            {
                regex_to_compile = current_params.patterns[0];
            }
        }
        else
        {
            // No patterns - shouldn't reach here due to earlier check
            goto cleanup;
        }

        // Compile the regex
        int rflags = REG_EXTENDED | REG_NEWLINE | (current_params.case_sensitive ? 0 : REG_ICASE);
        int ret = regcomp(&compiled_regex_local, regex_to_compile, rflags);

        if (ret != 0)
        {
            char ebuf[256];
            regerror(ret, &compiled_regex_local, ebuf, sizeof(ebuf));
            fprintf(stderr, "krep: Regex compilation error: %s\n", ebuf);
            goto cleanup;
        }

        regex_compiled = true;
        current_params.compiled_regex = &compiled_regex_local;
    }

    // --- Execute Search ---

    // Select and run the appropriate search algorithm
    search_func_t search_algo = select_search_algorithm(&current_params);

    // Perform search and collect results
    final_count = search_algo(&current_params, text, text_len, matches);

    // Determine final result based on matches found
    bool match_found = false;
    size_t max_count = current_params.max_count; // Get max_count

    // Adjust final_count based on max_count if necessary
    if (max_count != SIZE_MAX && final_count > max_count)
    {
        final_count = max_count;
    }
    // Adjust matches->count if tracking positions
    if (matches && max_count != SIZE_MAX && matches->count > max_count)
    {
        matches->count = max_count;
    }

    if (current_params.count_lines_mode || current_params.count_matches_mode)
    {
        match_found = (final_count > 0);
    }
    else
    {
        match_found = (matches && matches->count > 0);
        if (match_found)
        {
            final_count = matches->count;
        }
    }

    result_code = match_found ? 0 : 1;

    // --- Print Results ---

    if (current_params.count_lines_mode || current_params.count_matches_mode)
    {
        printf("%" PRIu64 "\n", final_count);
    }
    else
    {
        // Print matches/lines if found
        if (result_code == 0 && matches)
        {
            // No need to sort for string search (single thread)
            print_matching_items(NULL, text, text_len, matches, &current_params); // Pass params
        }
        // Handle case where match was found but no positions recorded (e.g., empty regex match)
        else if (result_code == 0 && (!matches || matches->count == 0))
        {
            if (only_matching)
            {
                // Print empty match for -o (consistent with grep)
                puts("");
            }
            else
            {
                // Print the whole (empty) line
                puts("");
            }
        }
    }

cleanup:
    // --- Cleanup ---
    if (regex_compiled)
    {
        regfree(&compiled_regex_local);
    }
    free(combined_regex_pattern);
    match_result_free(matches);
    // Free the Aho-Corasick trie if it was built locally
    if (local_ac_trie)
    {
        ac_trie_free(local_ac_trie);
    }

    return result_code;
}

// Global thread pool
static thread_pool_t *global_thread_pool = NULL;

// Initialize the global thread pool with auto-detected core count
static void init_global_thread_pool(int requested_thread_count)
{
    if (global_thread_pool == NULL)
    {
        global_thread_pool = thread_pool_init(requested_thread_count);
        if (!global_thread_pool)
        {
            fprintf(stderr, "Failed to initialize thread pool. Using single-threaded mode.\n");
        }
    }
}

// Clean up the global thread pool
static void KREP_UNUSED cleanup_global_thread_pool()
{
    if (global_thread_pool)
    {
        thread_pool_destroy(global_thread_pool);
        global_thread_pool = NULL;
    }
}

int search_file(const search_params_t *params, const char *filename, int requested_thread_count)
{
    search_params_t current_params = *params;
    ac_trie_t *local_ac_trie = NULL; // Pointer for locally built trie

    int result_code = 1;                         // Default: no match found
    int fd = -1;                                 // File descriptor
    struct stat file_stat;                       // File stats
    size_t file_size = 0;                        // File size
    char *file_data = MAP_FAILED;                // Mapped file data
    bool data_is_malloced = false;               // Flag to indicate if file_data was malloced
    match_result_t *global_matches = NULL;       // Global result collection
    pthread_t *threads = NULL;                   // Thread handles
    thread_data_t *thread_args = NULL;           // Thread arguments
    regex_t compiled_regex_local;                // For local regex compilation
    char *combined_regex_pattern = NULL;         // For combined regex patterns
    int actual_thread_count = 0;                 // Number of threads to actually use
    uint64_t final_count = 0;                    // Total count of lines or matches
    size_t max_count = current_params.max_count; // Get max_count

    // Validate patterns for literal search (not for regex)
    if (!current_params.use_regex)
    {
        for (size_t i = 0; i < current_params.num_patterns; ++i)
        {
            // Check for empty pattern - only allowed if there's a single pattern
            if (current_params.pattern_lens[i] == 0)
            {
                if (current_params.num_patterns > 1)
                {
                    fprintf(stderr, "krep: %s: Error: Empty pattern provided for literal search with multiple patterns.\n", filename);
                    return 2;
                }
                // Single empty pattern is allowed, continue to next pattern
                continue;
            }

            // Check for pattern length limit
            if (current_params.pattern_lens[i] > MAX_PATTERN_LENGTH)
            {
                fprintf(stderr, "krep: %s: Error: Pattern '%s' too long (max %d).\n",
                        filename, current_params.patterns[i], MAX_PATTERN_LENGTH);
                return 2;
            }
        }
    }

    // Input from stdin
    if (strcmp(filename, "-") == 0)
    {
        // Read from stdin into a dynamically growing buffer
        size_t buffer_size = 4 * 1024 * 1024; // Start with 4MB
        size_t used_size = 0;
        char *buffer = malloc(buffer_size);
        if (!buffer)
        {
            fprintf(stderr, "krep: Memory allocation failed for stdin buffer\n");
            return 2;
        }

        // Read stdin in chunks
        size_t read_chunk_size = 65536; // 64KB chunks
        size_t bytes_read;
        while ((bytes_read = fread(buffer + used_size, 1, read_chunk_size, stdin)) > 0)
        {
            used_size += bytes_read;
            // Expand buffer if needed
            if (used_size + read_chunk_size > buffer_size)
            {
                buffer_size *= 2;
                char *new_buffer = realloc(buffer, buffer_size);
                if (!new_buffer)
                {
                    fprintf(stderr, "krep: Memory reallocation failed for stdin buffer\n");
                    free(buffer);
                    return 2;
                }
                buffer = new_buffer;
            }
        }
        if (ferror(stdin))
        {
            fprintf(stderr, "krep: Error reading from stdin: %s\n", strerror(errno));
            free(buffer);
            return 2;
        }

        // Null-terminate the buffer for search_string
        // Realloc to exact size + 1 for null terminator
        char *final_buffer = realloc(buffer, used_size + 1);
        if (!final_buffer)
        {
            fprintf(stderr, "krep: Memory reallocation failed for final stdin buffer\n");
            free(buffer);
            return 2;
        }
        buffer = final_buffer;
        buffer[used_size] = '\0';

        // Need to build AC trie here too if needed for stdin search
        bool needs_ac_trie_stdin = (current_params.num_patterns > 1 && !current_params.use_regex);
        if (needs_ac_trie_stdin)
        {
            local_ac_trie = ac_trie_build(&current_params);
            if (!local_ac_trie)
            {
                fprintf(stderr, "krep: Error building Aho-Corasick trie for stdin.\n");
                free(buffer);
                return 2;
            }
            current_params.ac_trie = local_ac_trie;
        }

        // Search the buffer using search_string logic (single-threaded for stdin)
        // search_string will now use the pre-built trie if current_params.ac_trie is set
        result_code = search_string(&current_params, buffer);

        // Cleanup for stdin
        free(buffer);
        if (local_ac_trie)
        { // Free trie built for stdin
            ac_trie_free(local_ac_trie);
        }
        return result_code;
    }

    // --- Regular File Handling ---
    fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        fprintf(stderr, "krep: %s: %s\n", filename, strerror(errno));
        return 2;
    }
    if (fstat(fd, &file_stat) == -1)
    {
        fprintf(stderr, "krep: %s: %s\n", filename, strerror(errno));
        close(fd);
        return 2;
    }
    file_size = file_stat.st_size;

    // --- Handle Empty File ---
    if (file_size == 0)
    {
        close(fd);
        bool empty_match = false;
        bool needs_ac_trie_empty = (current_params.num_patterns > 1 && !current_params.use_regex);

        // Temporarily build trie just to check root outputs for empty pattern
        if (needs_ac_trie_empty)
        {
            ac_trie_t *temp_trie = ac_trie_build(&current_params);
            if (ac_trie_root_has_outputs(temp_trie))
            {
                empty_match = true;
            }
            if (temp_trie)
                ac_trie_free(temp_trie);
        }
        // Check regex empty match
        else if (current_params.use_regex)
        {
            // Compile regex temporarily to check for empty match
            regex_t temp_regex;
            const char *regex_to_compile = NULL;
            char *temp_combined_pattern = NULL;
            if (current_params.num_patterns > 1)
            {
                size_t total_len = 0;
                for (size_t i = 0; i < current_params.num_patterns; ++i)
                    total_len += current_params.pattern_lens[i] + 3;
                temp_combined_pattern = malloc(total_len + 1); // +1 for null
                if (temp_combined_pattern)
                {
                    char *ptr = temp_combined_pattern;
                    for (size_t i = 0; i < current_params.num_patterns; ++i)
                    {
                        ptr += sprintf(ptr, "(%s)", current_params.patterns[i]);
                        if (i < current_params.num_patterns - 1)
                            ptr += sprintf(ptr, "|");
                    }
                    *ptr = '\0'; // Null terminate
                    regex_to_compile = temp_combined_pattern;
                } // else: proceed with first pattern, might be inaccurate but avoids error
            }
            else if (current_params.num_patterns == 1)
            {
                regex_to_compile = current_params.patterns[0];
            }
            else
            { // No patterns
                free(temp_combined_pattern);
                return 1;
            }

            int rflags = REG_EXTENDED | REG_NEWLINE | (current_params.case_sensitive ? 0 : REG_ICASE);
            if (regcomp(&temp_regex, regex_to_compile, rflags) == 0)
            {
                regmatch_t m;
                if (regexec(&temp_regex, "", 1, &m, 0) == 0 && m.rm_so == 0 && m.rm_eo == 0)
                {
                    empty_match = true;
                }
                regfree(&temp_regex);
            }
            free(temp_combined_pattern);
        }
        // Check single literal empty pattern
        else if (current_params.num_patterns == 1 && current_params.pattern_lens[0] == 0)
        {
            empty_match = true;
        }

        if (empty_match)
        {
            if (current_params.count_lines_mode || current_params.count_matches_mode)
            {
                printf("%s:1\n", filename); // Print count 1
            }
            else if (only_matching)
            {                               // -o (global flag)
                printf("%s::\n", filename); // Print filename:: for empty match
            }
            else
            {                              // default
                printf("%s:\n", filename); // Print filename: followed by empty line
            }
            atomic_store(&global_match_found_flag, true); // Signal match found for -r
            return 0;                                     // Match found
        }
        else
        {
            if (current_params.count_lines_mode || current_params.count_matches_mode)
                printf("%s:0\n", filename); // Print count 0
            return 1;                       // No match
        }
    }

    // Check if pattern is longer than file (only for single literal search)
    if (!current_params.use_regex && current_params.num_patterns == 1 && current_params.pattern_lens[0] > file_size)
    {
        close(fd);
        if (current_params.count_lines_mode || current_params.count_matches_mode)
            printf("%s:0\n", filename);
        return 1; // No match possible
    }

    // --- Build Aho-Corasick Trie (if needed, once for the file) ---
    bool needs_ac_trie_file = (current_params.num_patterns > 1 && !current_params.use_regex);
    if (needs_ac_trie_file)
    {
        local_ac_trie = ac_trie_build(&current_params);
        if (!local_ac_trie)
        {
            fprintf(stderr, "krep: Error building Aho-Corasick trie for %s.\n", filename);
            result_code = 2;
            goto cleanup_file;
        }
        current_params.ac_trie = local_ac_trie; // Assign to the mutable params copy
    }

    // --- Compile Regex (if needed, once for the file) ---
    if (current_params.use_regex)
    {
        const char *regex_to_compile = NULL;
        if (current_params.num_patterns > 1)
        {
            // Combine multiple regex patterns with '|'
            size_t total_len = 0;
            for (size_t i = 0; i < current_params.num_patterns; ++i)
            {
                // Add 6 for wrapping with (\b...\b)
                total_len += current_params.pattern_lens[i] + (current_params.whole_word ? 6 : 2) + 1; // () or (\b...\b) + |
            }
            combined_regex_pattern = malloc(total_len + 1); // +1 for null terminator
            if (!combined_regex_pattern)
            {
                fprintf(stderr, "krep: %s: Failed to allocate memory for combined regex.\n", filename);
                close(fd);
                return 2;
            }
            char *ptr = combined_regex_pattern;
            for (size_t i = 0; i < current_params.num_patterns; ++i)
            {
                if (current_params.whole_word)
                    ptr += sprintf(ptr, "(\\b%s\\b)", current_params.patterns[i]);
                else
                    ptr += sprintf(ptr, "(%s)", current_params.patterns[i]);
                if (i < current_params.num_patterns - 1)
                {
                    ptr += sprintf(ptr, "|");
                }
            }
            *ptr = '\0'; // Null terminate
            regex_to_compile = combined_regex_pattern;
        }
        else if (current_params.num_patterns == 1)
        {
            if (current_params.whole_word)
            {
                size_t len = strlen(current_params.patterns[0]);
                char *tmp = malloc(len + 7); // (\b) + pattern + (\b) + null
                if (!tmp)
                {
                    fprintf(stderr, "krep: Failed to allocate memory for regex pattern.\n");
                    close(fd);
                    return 2;
                }
                sprintf(tmp, "\\b%s\\b", current_params.patterns[0]);
                regex_to_compile = tmp;
                free(combined_regex_pattern); // In case it was set
                combined_regex_pattern = tmp; // So it gets freed later
            }
            else
            {
                regex_to_compile = current_params.patterns[0]; // Ensure correct pattern is used
            }
        }
        else
        { // Should not happen due to earlier check
            close(fd);
            return 1;
        }

        int rflags = REG_EXTENDED | REG_NEWLINE | (current_params.case_sensitive ? 0 : REG_ICASE);
        int ret = regcomp(&compiled_regex_local, regex_to_compile, rflags);
        if (ret != 0)
        {
            char ebuf[256];
            regerror(ret, &compiled_regex_local, ebuf, sizeof(ebuf));
            fprintf(stderr, "krep: Regex compilation error for %s: %s\n", filename, ebuf);
            close(fd);
            free(combined_regex_pattern);
            return 2;
        }
        // Modify the mutable copy of params
        search_params_t mutable_params = current_params;
        mutable_params.compiled_regex = &compiled_regex_local;
        current_params = mutable_params; // Update current_params to use for threads
        // Ensure local_ac_trie is NULL if regex is used
        if (local_ac_trie)
        {
            ac_trie_free(local_ac_trie);
            local_ac_trie = NULL;
            current_params.ac_trie = NULL;
        }
    }

#if defined(POSIX_FADV_SEQUENTIAL) && !defined(__APPLE__)
    // Hint the kernel about sequential access to encourage readahead
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // --- Memory Map or Read File ---
    // Optimization: For small files, use read() to avoid mmap overhead and page faults.
    // For regex searches, always use malloc+read to ensure null-termination,
    // because regexec with REG_STARTEND may read beyond the specified rm_eo boundary.
    if (file_size < 65536 || current_params.use_regex) // 64KB threshold or regex mode
    {
        file_data = malloc(file_size + 1); // +1 for safety/null-term if needed
        if (!file_data)
        {
            fprintf(stderr, "krep: %s: malloc failed: %s\n", filename, strerror(errno));
            close(fd);
            if (current_params.use_regex && current_params.compiled_regex == &compiled_regex_local)
                regfree(&compiled_regex_local);
            free(combined_regex_pattern);
            result_code = 2;
            goto cleanup_file;
        }
        
        ssize_t bytes_read = 0;
        size_t total_read = 0;
        while (total_read < file_size)
        {
            bytes_read = read(fd, file_data + total_read, file_size - total_read);
            if (bytes_read < 0)
            {
                if (errno == EINTR) continue;
                fprintf(stderr, "krep: %s: read failed: %s\n", filename, strerror(errno));
                free(file_data);
                close(fd);
                if (current_params.use_regex && current_params.compiled_regex == &compiled_regex_local)
                    regfree(&compiled_regex_local);
                free(combined_regex_pattern);
                result_code = 2;
                goto cleanup_file;
            }
            if (bytes_read == 0) break; // Unexpected EOF
            total_read += bytes_read;
        }
        file_data[file_size] = '\0'; // Null terminate for safety
        data_is_malloced = true;
    }
    else
    {
        // Use mmap for larger files
        int mmap_base_flags = MAP_PRIVATE;
        file_data = MAP_FAILED; // Initialize file_data

#ifdef MAP_POPULATE
        // Try with MAP_POPULATE first
        int mmap_flags_populate = mmap_base_flags | MAP_POPULATE;
        file_data = mmap(NULL, file_size, PROT_READ, mmap_flags_populate, fd, 0);

        // If MAP_POPULATE failed, try without it
        if (file_data == MAP_FAILED && errno == ENOTSUP) // Check if MAP_POPULATE is specifically not supported
        {
            // fprintf(stderr, "krep: %s: mmap with MAP_POPULATE not supported, retrying without...\n", filename);
            file_data = mmap(NULL, file_size, PROT_READ, mmap_base_flags, fd, 0);
        }
        else if (file_data == MAP_FAILED)
        {
            fprintf(stderr, "krep: %s: mmap with MAP_POPULATE failed (%s), retrying without...\n", filename, strerror(errno));
            file_data = mmap(NULL, file_size, PROT_READ, mmap_base_flags, fd, 0);
        }
#else
        // MAP_POPULATE not defined, just call mmap without it
        file_data = mmap(NULL, file_size, PROT_READ, mmap_base_flags, fd, 0);
#endif

        // Check if mmap failed even after potential fallback
        if (file_data == MAP_FAILED)
        {
            fprintf(stderr, "krep: %s: mmap: %s\n", filename, strerror(errno));
            close(fd);
            if (current_params.use_regex && current_params.compiled_regex == &compiled_regex_local)
                regfree(&compiled_regex_local);
            free(combined_regex_pattern);
            // No need to free global_matches, threads, thread_args here, handled by goto cleanup_file
            result_code = 2;
            goto cleanup_file; // Use goto to ensure proper cleanup
        }

        // Advise the kernel about expected access pattern
        int madvise_ret = madvise(file_data, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
        if (madvise_ret != 0)
        {
            int madvise_err = errno;
            if (!atomic_exchange(&madvise_warning_emitted, true))
            {
                fprintf(stderr, "krep: %s: Warning: madvise failed: %s (future warnings suppressed)\n",
                        filename, strerror(madvise_err));
            }
            // Continue execution since this is just an optimization
        }
    }

    close(fd); // Close file descriptor after mmap
    fd = -1;

    // --- Determine Thread Count and Chunking ---
    if (requested_thread_count == 0)
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        actual_thread_count = (cores > 0) ? (int)cores : 1;
    }
    else
    {
        actual_thread_count = requested_thread_count;
    }
    int max_threads_by_size = (file_size > 0) ? (int)((file_size + MIN_CHUNK_SIZE - 1) / MIN_CHUNK_SIZE) : 1;
    if (actual_thread_count > max_threads_by_size && max_threads_by_size > 0)
    {
        actual_thread_count = max_threads_by_size;
    }
    if (actual_thread_count <= 0)
        actual_thread_count = 1;

    // --- Initialize Threading Resources ---
    threads = malloc(actual_thread_count * sizeof(pthread_t));
    thread_args = malloc(actual_thread_count * sizeof(thread_data_t));
    if (threads)
        memset(threads, 0, actual_thread_count * sizeof(pthread_t));

    if (!threads || !thread_args)
    {
        perror("krep: Cannot allocate thread resources");
        result_code = 2;
        goto cleanup_file;
    }

    // Allocate global results structure if tracking positions
    if (current_params.track_positions)
    {
        uint64_t initial_cap = (file_size / 1000 > 1000) ? file_size / 1000 : 1000;
        global_matches = match_result_init(initial_cap);
        if (!global_matches)
        {
            fprintf(stderr, "krep: Error: Cannot allocate global match results for %s.\n", filename);
            result_code = 2;
            goto cleanup_file;
        }
    }

    // --- Launch Threads ---
    size_t chunk_size_calc = (file_size + actual_thread_count - 1) / actual_thread_count;
    if (chunk_size_calc == 0 && file_size > 0)
        chunk_size_calc = file_size;
    // Ensure minimum chunk size
    if (chunk_size_calc < MIN_CHUNK_SIZE && file_size > MIN_CHUNK_SIZE)
    {
        chunk_size_calc = MIN_CHUNK_SIZE;
        // Recalculate thread count based on adjusted chunk size
        actual_thread_count = (file_size + chunk_size_calc - 1) / chunk_size_calc;
        if (actual_thread_count <= 0)
            actual_thread_count = 1;
        // Reallocate thread resources if count changed significantly (optional, could just use max)
        // For simplicity, we assume initial allocation was sufficient or handle errors later.
    }

    size_t current_pos = 0;
    int threads_launched = 0;
    // Calculate max pattern length for overlap (only for literal search)
    size_t max_literal_pattern_len = 0;
    if (!current_params.use_regex)
    {
        for (size_t i = 0; i < current_params.num_patterns; ++i)
        {
            if (current_params.pattern_lens[i] > max_literal_pattern_len)
            {
                max_literal_pattern_len = current_params.pattern_lens[i];
            }
        }
    }

    // Determine how many threads to use based on file size and available cores
    int available_cores = requested_thread_count > 0 ? requested_thread_count : sysconf(_SC_NPROCESSORS_ONLN);
    if (available_cores <= 0)
        available_cores = 1;

    // Calculate optimal number of threads: min(cores, max(1, file_size/(4MB)))
    // This scales threads with file size, but caps at CPU core count
    int optimal_threads = 1;
    if (file_size > 0)
    {
        size_t chunk_threshold = 4 * 1024 * 1024; // 4MB per thread minimum
        optimal_threads = file_size / chunk_threshold;
        if (optimal_threads > available_cores)
            optimal_threads = available_cores;
        if (optimal_threads < 1)
            optimal_threads = 1;
    }

    // Initialize the global thread pool if needed
    init_global_thread_pool(optimal_threads);

    // Preselect search algorithm once to avoid redundant decisions inside each worker
    search_func_t preselected_algo = select_search_algorithm(&current_params);

    for (int i = 0; i < actual_thread_count; ++i)
    {
        if (current_pos >= file_size)
        {
            actual_thread_count = i; // Update actual count
            break;
        }

        thread_args[i].thread_id = i;
        thread_args[i].params = &current_params; // Pass params containing the pre-built trie
        thread_args[i].chunk_start = file_data + current_pos;
        thread_args[i].search_algo = preselected_algo;

        size_t this_chunk_len = (current_pos + chunk_size_calc > file_size) ? (file_size - current_pos) : chunk_size_calc;

        // Overlap needed for literal patterns. Regex handled differently (often needs no overlap or different logic).
        size_t overlap = (!current_params.use_regex && max_literal_pattern_len > 0 && i < actual_thread_count - 1) ? max_literal_pattern_len - 1 : 0;
        size_t effective_chunk_len = (current_pos + this_chunk_len + overlap > file_size) ? (file_size - current_pos) : (this_chunk_len + overlap);

        // Ensure chunk length isn't zero if there's still data
        if (effective_chunk_len == 0 && current_pos < file_size)
        {
            effective_chunk_len = file_size - current_pos;
        }

        thread_args[i].chunk_len = effective_chunk_len;
        thread_args[i].local_result = NULL;
        thread_args[i].count_result = 0;
        thread_args[i].error_flag = false;

        if (effective_chunk_len > 0)
        {
            // Use search_chunk_thread which handles multiple patterns via Aho-Corasick or Regex
            if (global_thread_pool)
            {
                if (!thread_pool_submit(global_thread_pool, search_chunk_thread, &thread_args[i]))
                {
                    fprintf(stderr, "krep: Failed to submit task for thread %d\n", i);
                    // Handle error: maybe reduce thread count or abort
                    thread_args[i].error_flag = true; // Mark thread data as errored
                }
            }
            else
            {
                int rc = pthread_create(&threads[i], NULL, search_chunk_thread, &thread_args[i]);
                if (rc)
                {
                    fprintf(stderr, "krep: Error creating thread %d: %s\n", i, strerror(rc));
                    // Handle error: maybe reduce thread count or abort
                    threads[i] = 0;                   // Mark as not created
                    thread_args[i].error_flag = true; // Mark thread data as errored
                    continue;                         // Try launching fewer threads
                }
            }
            threads_launched++;
        }
        else
        {
            threads[i] = 0; // Mark as not launched
        }
        current_pos += this_chunk_len; // Advance by non-overlapped length
    }
    actual_thread_count = threads_launched;

    // Wait for all tasks to complete
    if (global_thread_pool)
    {
        thread_pool_wait_all(global_thread_pool);
    }

    // --- Wait for Threads and Aggregate Results ---
    bool merge_error = false;
    for (int i = 0; i < actual_thread_count; ++i)
    {
        // Skip the pthread_join logic if using thread pool (tasks are already complete)
        // Only try to join if not using thread pool and thread was actually created
        if (!global_thread_pool && threads[i] != 0)
        {
            int rc = pthread_join(threads[i], NULL);
            if (rc)
            {
                fprintf(stderr, "krep: Error joining thread %d: %s\n", i, strerror(rc));
                result_code = 2;
            }
        }

        if (thread_args[i].error_flag)
        {
            result_code = 2;
        }

        // Always process results from thread_args, regardless of how the thread was executed
        if (result_code != 2 && !merge_error)
        {
            // Sum counts (lines or matches). Note: Line count might be slightly off at boundaries.
            uint64_t thread_count = thread_args[i].count_result;
            if (max_count != SIZE_MAX)
            {
                uint64_t remaining_limit = (final_count >= max_count) ? 0 : max_count - final_count;
                if (thread_count > remaining_limit)
                {
                    thread_count = remaining_limit; // Cap thread count contribution
                }
            }
            final_count += thread_count;

            // Merge position results if tracking, respecting max_count
            if (current_params.track_positions && global_matches && thread_args[i].local_result)
            {
                bool merge_ok = true;
                if (max_count == SIZE_MAX)
                {
                    size_t chunk_offset = (size_t)(thread_args[i].chunk_start - file_data);
                    merge_ok = match_result_merge(global_matches, thread_args[i].local_result, chunk_offset);
                }
                else if (global_matches->count < max_count)
                {
                    size_t chunk_offset = (size_t)(thread_args[i].chunk_start - file_data);
                    uint64_t remaining_limit = max_count - global_matches->count;
                    merge_ok = match_result_merge_limited(global_matches,
                                                          thread_args[i].local_result,
                                                          chunk_offset,
                                                          remaining_limit);
                }

                if (!merge_ok)
                {
                    fprintf(stderr, "krep: %s: Failed to merge match result from thread %d.\n", filename, i);
                    merge_error = true;
                }

                match_result_free(thread_args[i].local_result); // Free local result after merging/skip
                thread_args[i].local_result = NULL;
            }
            else if (thread_args[i].local_result)
            {
                match_result_free(thread_args[i].local_result);
                thread_args[i].local_result = NULL;
            }

            if (merge_error)
            {
                result_code = 2;
                // Continue cleanup but don't process further results
            }
        }
    }

    // --- Final Processing and Output ---
    if (result_code != 2)
    {
        // Determine final result code based on aggregated count/matches
        result_code = (final_count > 0) ? 0 : 1;
        if (result_code == 0)
            atomic_store(&global_match_found_flag, true); // Signal match found for -r

        if (current_params.count_lines_mode || current_params.count_matches_mode)
        {
            printf("%s:%" PRIu64 "\n", filename, final_count);
        }
        else if (result_code == 0 && global_matches)
        {
            if (global_matches->count > 1)
            {
                qsort(global_matches->positions, global_matches->count, sizeof(match_position_t), compare_match_positions);
            }

            // Print matching lines/parts, respecting max_count via print_matching_items
            print_matching_items(filename, file_data, file_size, global_matches, &current_params); // Pass params
        }
        // Handle case where match was found but no positions recorded (e.g., empty regex match)
        else if (result_code == 0 && (!global_matches || global_matches->count == 0))
        {
            if (only_matching)
            {
                printf("%s:1:\n", filename); // Line number 1, empty match
            }
            else
            {
                printf("%s:\n", filename); // Empty line
            }
        }
    }

cleanup_file:
    // --- Cleanup Resources ---
    if (file_data != MAP_FAILED) {
        if (data_is_malloced)
            free(file_data);
        else
            munmap(file_data, file_size);
    }
    if (current_params.use_regex && current_params.compiled_regex == &compiled_regex_local)
        regfree(&compiled_regex_local);
    free(combined_regex_pattern);
    match_result_free(global_matches);
    free(threads);
    free(thread_args);
    if (fd != -1)
        close(fd);
    // Free the Aho-Corasick trie if it was built for this file
    if (local_ac_trie)
    {
        ac_trie_free(local_ac_trie);
    }

    return result_code;
}

// --- Recursive Directory Search ---

// Check if a directory name should be skipped
static bool should_skip_directory(const char *dirname)
{
    // Skip hidden directories starting with '.' (in addition to "." and "..")
    if (dirname[0] == '.' && strcmp(dirname, ".") != 0 && strcmp(dirname, "..") != 0)
    {
        return true;
    }
    // Check against the predefined list of directories to skip
    for (size_t i = 0; i < num_skip_directories; ++i)
    {
        if (strcmp(dirname, skip_directories[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

// Check if a file extension should be skipped
static bool should_skip_extension(const char *filename)
{
    // Find the last dot in the filename
    const char *dot = strrchr(filename, '.');

    // If no dot, or dot is at the beginning (hidden file), or dot is the last character,
    // it's not an extension we check here.
    if (!dot || dot == filename || *(dot + 1) == '\0')
    {
        return false; // Not an extension we care about
    }

    // Check for .min. files (minified files like .min.js, .min.css)
    // These are common enough to merit a special check
    const char *min_ext = strstr(dot, ".min.");
    if (min_ext && min_ext == dot)
    {
        return true; // Skip minified files
    }

    // Check against the predefined list
    for (size_t i = 0; i < num_skip_extensions; ++i)
    {
        if (strcasecmp(dot, skip_extensions[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

// Check if a file appears to be binary
static bool is_binary_file(const char *filepath)
{
    // Open file and check for binary content
    FILE *f = fopen(filepath, "rb");
    if (!f)
    {
        return false; // Treat fopen error as non-binary (might be permission issue)
    }

    char buffer[BINARY_CHECK_BUFFER_SIZE];
    // Read a chunk from the beginning of the file
    size_t bytes_read = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);

    if (bytes_read == 0)
        return false; // Empty file is not binary

    // Check if a null byte exists within the read buffer
    return memchr(buffer, '\0', bytes_read) != NULL;
}

// Recursive directory search function
// Note: Directory traversal itself remains serial. Parallelism happens within search_file.
int search_directory_recursive(const char *base_dir, const search_params_t *params, int thread_count)
{
    // Try opening the directory
    DIR *dir = opendir(base_dir);
    if (!dir)
    {
        // Better error handling: print more informative message for common errors
        if (errno == EACCES)
        {
            fprintf(stderr, "krep: %s: Permission denied\n", base_dir);
        }
        else if (errno != ENOENT)
        { // Still silent for not found
            fprintf(stderr, "krep: %s: %s\n", base_dir, strerror(errno));
        }
        // Return 0 errors for permission/not found, 1 otherwise
        return (errno == EACCES || errno == ENOENT) ? 0 : 1;
    }

    struct dirent *entry;       // Structure to hold directory entry info
    int total_errors = 0;       // Accumulator for errors during recursion
    char path_buffer[PATH_MAX]; // Buffer to construct full paths

    // Read directory entries one by one
    while ((entry = readdir(dir)) != NULL)
    {
        // Skip "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        // Construct the full path for the entry - more robust path joining
        int path_len;
        if (base_dir[strlen(base_dir) - 1] == '/')
        {
            path_len = snprintf(path_buffer, sizeof(path_buffer), "%s%s", base_dir, entry->d_name);
        }
        else
        {
            path_len = snprintf(path_buffer, sizeof(path_buffer), "%s/%s", base_dir, entry->d_name);
        }

        // Check for path construction errors (e.g., path too long)
        if (path_len < 0 || (size_t)path_len >= sizeof(path_buffer))
        {
            fprintf(stderr, "krep: Error constructing path for %s/%s (too long?)\n", base_dir, entry->d_name);
            total_errors++;
            continue;
        }

        struct stat entry_stat; // Structure to hold file status info
        // Use lstat to get info about the entry itself (doesn't follow symlinks)
        if (lstat(path_buffer, &entry_stat) == -1)
        {
            // Ignore "No such file or directory" errors (e.g., broken symlink), report others
            if (errno != ENOENT)
            {
                fprintf(stderr, "krep: %s: %s\n", path_buffer, strerror(errno));
                total_errors++;
            }
            continue;
        }

        // If the entry is a directory:
        if (S_ISDIR(entry_stat.st_mode))
        {
            // Check if this directory should be skipped
            if (should_skip_directory(entry->d_name))
            {
                continue; // Skip this directory
            }
            // Otherwise, recurse into the subdirectory
            total_errors += search_directory_recursive(path_buffer, params, thread_count);
        }
        // If the entry is a regular file:
        else if (S_ISREG(entry_stat.st_mode))
        {
            // Check if the file should be skipped based on extension
            if (should_skip_extension(entry->d_name))
            {
                continue; // Skip this file
            }

            // Don't check binary files too aggressively as it might miss valid text files
            // Only check files larger than a certain threshold
            if (entry_stat.st_size > 1024 * 1024 && is_binary_file(path_buffer))
            {
                continue; // Skip this file - it's binary and large
            }

            // Otherwise, search the file using search_file (which handles parallelism)
            int file_result = search_file(params, path_buffer, thread_count);
            // If search_file returns 2, it indicates an error
            if (file_result == 2)
            {
                total_errors++;
            }
            // Note: global_match_found_flag is set within search_file if matches are found
        }
        // Ignore other file types (symlinks are not followed by lstat, sockets, pipes, etc.)
    }

    closedir(dir);       // Close the directory stream
    return total_errors; // Return the total count of errors encountered
}

// --- Main Entry Point ---

// Exclude main if TESTING is defined (for linking with test harness)
#if !defined(TESTING)
int main(int argc, char *argv[])
{
    // --- Argument Parsing State ---
    search_params_t params = {0}; // Initialize search parameters
    params.case_sensitive = true; // Default
    params.num_patterns = 0;
    params.use_regex = false; // Default to literal search

    // Temporary storage for multiple patterns from -e
    char *pattern_args[MAX_PATTERN_LENGTH]; // Store pointers to patterns from argv
    size_t pattern_lens[MAX_PATTERN_LENGTH];
    size_t num_patterns_found = 0;

    // Add an array to track which patterns were dynamically allocated and need to be freed
    bool pattern_needs_free[MAX_PATTERN_LENGTH] = {false};

    char *target_arg = NULL;                 // The file, directory, or string to search
    bool count_only_flag = false;            // Flag for -c
    bool string_mode = false;                // Flag for -s (search string instead of file)
    bool recursive_mode = false;             // Flag for -r (recursive directory search)
    int thread_count = DEFAULT_THREAD_COUNT; // Thread count (0 = auto)
    const char *color_when = "auto";         // Color output control ('auto', 'always', 'never')

    // --- getopt_long Setup ---
    struct option long_options[] = {
        {"color", optional_argument, 0, 'C'},     // --color[=WHEN]
        {"no-simd", no_argument, 0, 'S'},         // --no-simd
        {"help", no_argument, 0, 'h'},            // --help
        {"version", no_argument, 0, 'v'},         // --version
        {"fixed-strings", no_argument, 0, 'F'},   // --fixed-strings, same as default
        {"regexp", required_argument, 0, 'e'},    // Treat -e as --regexp for consistency
        {"max-count", required_argument, 0, 'm'}, // --max-count=NUM option
        {0, 0, 0, 0}                              // Terminator
    };
    int option_index = 0;
    int opt;

    // Initialize max_count to SIZE_MAX (no limit)
    params.max_count = SIZE_MAX;

    // --- Parse Command Line Options ---
    while ((opt = getopt_long(argc, argv, "+e:f:icm:oEFrt:s:vhw", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 'i': // Case-insensitive
            params.case_sensitive = false;
            break;
        case 'c': // Count lines
            count_only_flag = true;
            break;
        case 'o': // Only matching parts
            only_matching = true;
            break;
        case 'm': // Max count
        {
            char *endptr = NULL;
            errno = 0;
            long val = strtol(optarg, &endptr, 10);
            if (errno != 0 || optarg == endptr || *endptr != '\0' || val < 0)
            {
                fprintf(stderr, "krep: Warning: Invalid number for max-count '%s'\n", optarg);
            }
            else
            {
                // Cast to size_t (always less than SIZE_MAX since we've verified val >= 0)
                params.max_count = (size_t)val;
            }
        }
        break;
        case 'E': // Use Extended Regex
            params.use_regex = true;
            break;
        case 'F': // Fixed strings (explicitly, same as default)
            params.use_regex = false;
            break;
        case 'r': // Recursive search
            recursive_mode = true;
            break;
        case 't': // Set thread count
        {
            char *endptr = NULL;
            errno = 0;
            long val = strtol(optarg, &endptr, 10);
            if (errno != 0 || optarg == endptr || *endptr != '\0' || val <= 0 || val > INT_MAX)
            {
                fprintf(stderr, "krep: Warning: Invalid thread count '%s', using default.\n", optarg);
                thread_count = DEFAULT_THREAD_COUNT;
            }
            else
            {
                thread_count = (int)val;
            }
        }
        break;
        case 's': // Search string mode
            string_mode = true;
            // optarg is the PATTERN for string mode.
            // Add it to the list of patterns.
            if (num_patterns_found < MAX_PATTERN_LENGTH)
            {
                pattern_args[num_patterns_found] = optarg;
                pattern_lens[num_patterns_found] = strlen(optarg);
                pattern_needs_free[num_patterns_found] = false; // Pattern is from argv, no free needed by this array
                num_patterns_found++;
            }
            else
            {
                fprintf(stderr, "krep: Error: Too many patterns specified.\n");
                for (size_t i = 0; i < num_patterns_found; ++i)
                {
                    if (pattern_needs_free[i])
                        free(pattern_args[i]);
                }
                return 2;
            }
            // STRING_TO_SEARCH (target_arg) will be the next non-option argument.
            break;
        case 'f': // Read patterns from file
        {
            FILE *pattern_file = fopen(optarg, "r");
            if (!pattern_file)
            {
                fprintf(stderr, "krep: Error: Cannot open pattern file: %s\n", optarg);
                return 2;
            }

            char line[MAX_PATTERN_LENGTH];
            while (fgets(line, sizeof(line), pattern_file) && num_patterns_found < MAX_PATTERN_LENGTH)
            {
                // Remove trailing newline if present
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n')
                    line[len - 1] = '\0';

                // Skip empty lines
                if (strlen(line) == 0)
                    continue;

                // Allocate storage for the pattern
                pattern_args[num_patterns_found] = strdup(line);
                if (!pattern_args[num_patterns_found])
                {
                    perror("krep: Error: Memory allocation failed for pattern");
                    fclose(pattern_file);
                    return 2;
                }
                pattern_lens[num_patterns_found] = strlen(pattern_args[num_patterns_found]);
                // Mark this pattern as needing to be freed
                pattern_needs_free[num_patterns_found] = true;
                num_patterns_found++;
            }
            fclose(pattern_file);

            if (num_patterns_found == 0)
            {
                fprintf(stderr, "krep: Error: No patterns found in file: %s\n", optarg);
                return 2;
            }
            break;
        }

        case 'v': // Version
            printf("krep v%s\n", VERSION);
#if KREP_USE_AVX2
            printf("SIMD: Compiled with AVX2 support.\n");
#elif KREP_USE_SSE42
            printf("SIMD: Compiled with SSE4.2 support.\n");
#elif KREP_USE_NEON
            printf("SIMD: Compiled with NEON support.\n");
#else
            printf("SIMD: Compiled without specific SIMD support.\n");
#endif
            printf("Max SIMD Pattern Length: %zu bytes\n", SIMD_MAX_PATTERN_LEN);
            return 0;
        case 'h': // Help
            print_usage(argv[0]);
            return 0;
        case 'e': // Specify pattern via option
            if (num_patterns_found < MAX_PATTERN_LENGTH)
            {
                pattern_args[num_patterns_found] = optarg;
                pattern_lens[num_patterns_found] = strlen(optarg);
                // These patterns come from argv, no need to free
                pattern_needs_free[num_patterns_found] = false;
                num_patterns_found++;
            }
            else
            {
                fprintf(stderr, "krep: Error: Too many patterns specified (max %d)\n", MAX_PATTERN_LENGTH);
                return 2;
            }
            break;

        case 'C': // --color option
            if (optarg == NULL || strcmp(optarg, "auto") == 0)
                color_when = "auto";
            else if (strcmp(optarg, "always") == 0)
                color_when = "always";
            else if (strcmp(optarg, "never") == 0)
                color_when = "never";
            else
            {
                fprintf(stderr, "krep: Error: Invalid argument for --color: %s\n", optarg);
                print_usage(argv[0]);
                return 2;
            }
            break;
        case 'S': // --no-simd option
            force_no_simd = true;
            break;
        case 'w': // Whole word
            params.whole_word = true;
            break;
        case '?': // Unknown option or missing argument from getopt
        default:  // Should not happen
            print_usage(argv[0]);
            return 2;
        }
    }

    // --- Finalize Parameter Setup ---

    // Determine color output setting
    if (strcmp(color_when, "always") == 0)
        color_output_enabled = true;
    else if (strcmp(color_when, "never") == 0)
        color_output_enabled = false;
    else                                              // "auto" (default)
        color_output_enabled = isatty(STDOUT_FILENO); // Enable only if stdout is a TTY

    // Get pattern argument(s)
    if (num_patterns_found == 0)
    {                      // No patterns from -e, -f, or -s
        if (optind < argc) // A non-option argument exists, this is the PATTERN
        {
            pattern_args[0] = argv[optind];
            pattern_lens[0] = strlen(pattern_args[0]);
            pattern_needs_free[0] = false; // Pattern is from argv
            num_patterns_found = 1;
            optind++;
        }
        else // No pattern argument provided at all
        {
            fprintf(stderr, "krep: Error: PATTERN argument missing.\n");
            print_usage(argv[0]);
            return 2;
        }
    }

    // Assign patterns to params struct
    params.patterns = (const char **)pattern_args; // Cast is safe as we won't modify argv content
    params.pattern_lens = pattern_lens;
    params.num_patterns = num_patterns_found;
    // For single pattern case, also set the legacy fields for compatibility
    if (num_patterns_found == 1)
    {
        params.pattern = params.patterns[0];
        params.pattern_len = params.pattern_lens[0];
    }

    // Get target argument (file, directory, or string to search)
    if (string_mode)
    {
        // In string mode, the next non-option argument is STRING_TO_SEARCH
        if (optind < argc)
        {
            target_arg = argv[optind];
            optind++;
        }
        else
        {
            // No non-option argument left for STRING_TO_SEARCH
            fprintf(stderr, "krep: Error: STRING_TO_SEARCH argument missing for -s.\n");
            for (size_t i = 0; i < num_patterns_found; ++i)
            {
                if (pattern_needs_free[i])
                    free(pattern_args[i]);
            }
            print_usage(argv[0]);
            return 2;
        }
    }
    else
    {
        // Not string mode, target is FILE/DIRECTORY or stdin
        if (optind < argc)
        { // A non-option argument exists for file/directory
            target_arg = argv[optind];
            optind++;
        }
        else
        {
            // No file/directory specified.
            // If a pattern was given and stdin is not a tty, input will be from stdin.
            // target_arg remains NULL for stdin.
            // If stdin is a tty, it's an error because no file/pipe is provided.
            if (num_patterns_found > 0 && isatty(STDIN_FILENO))
            {
                fprintf(stderr, "krep: Error: Target file/directory missing and no input from pipe/redirect.\n");
                for (size_t i = 0; i < num_patterns_found; ++i)
                {
                    if (pattern_needs_free[i])
                        free(pattern_args[i]);
                }
                print_usage(argv[0]);
                return 2;
            }
            // If num_patterns_found == 0, error was already caught.
            // If !isatty(STDIN_FILENO), target_arg is NULL (stdin).
        }
    }

    // Check for extra arguments
    if (optind < argc)
    {
        fprintf(stderr, "krep: Error: Extra arguments provided ('%s'...). \n", argv[optind]);
        print_usage(argv[0]);
        return 2;
    }

    // Validate incompatible options
    if (string_mode && recursive_mode)
    {
        fprintf(stderr, "krep: Error: Options -s (search string) and -r (recursive) cannot be used together.\n");
        print_usage(argv[0]);
        return 2;
    }

    // Set final counting/tracking modes in params
    params.count_lines_mode = count_only_flag && !only_matching;  // -c only
    params.count_matches_mode = count_only_flag && only_matching; // -co (internal concept, currently unused externally)
    // Track positions unless only counting lines (-c without -o)
    params.track_positions = !(count_only_flag && !only_matching);

    // If counting (-c) or printing only matches (-o), disable summary

    // Initialize thread pool early with the requested thread count
    init_global_thread_pool(thread_count);

    // --- Execute Search ---
    int exit_code = 1; // Default exit code: 1 (no match found)

    if (string_mode)
    {
        // Search the provided string argument
        exit_code = search_string(&params, target_arg);
    }
    else if (recursive_mode)
    {
        // Search recursively starting from the target directory
        struct stat target_stat;
        if (stat(target_arg, &target_stat) == -1)
        {
            fprintf(stderr, "krep: %s: %s\n", target_arg, strerror(errno));
            return 2; // Target does not exist or other stat error
        }
        if (!S_ISDIR(target_stat.st_mode))
        {
            fprintf(stderr, "krep: %s: Is not a directory (required for -r)\n", target_arg);
            return 2;
        }
        atomic_store(&global_match_found_flag, false); // Reset global flag
        int errors = search_directory_recursive(target_arg, &params, thread_count);
        if (errors > 0)
        {
            fprintf(stderr, "krep: Encountered %d errors during recursive search.\n", errors);
            exit_code = 2; // Exit code 2 if errors occurred
        }
        else
        {
            exit_code = atomic_load(&global_match_found_flag) ? 0 : 1; // 0 if matches found, 1 otherwise
        }
    }
    else
    { // Single target (file or stdin)
        // If target_arg is NULL, it means stdin - set to "-" for consistency
        if (target_arg == NULL)
            target_arg = "-";

        // search_file handles stdin internally if target_arg is "-"
        struct stat target_stat;
        // If not stdin, check if it's a directory without -r
        if (strcmp(target_arg, "-") != 0 && stat(target_arg, &target_stat) == 0 && S_ISDIR(target_stat.st_mode))
        {
            fprintf(stderr, "krep: %s: Is a directory (use -r to search directories)\n", target_arg);
            return 2;
        }
        // Call search_file (handles stdin via target_arg == "-")
        exit_code = search_file(&params, target_arg, thread_count);
    }

    // Clean up thread pool before exiting
    cleanup_global_thread_pool();

    // Cleanup before exit - free any memory allocated for patterns read from file
    if (num_patterns_found > 0)
    {
        for (size_t i = 0; i < num_patterns_found; i++)
        {
            if (pattern_needs_free[i] && pattern_args[i])
            {
                free(pattern_args[i]);
            }
        }
    }

    // Return the final exit code (0=match, 1=no match, 2=error)
    return exit_code;
}
#endif // !defined(TESTING)

// Add near the other search functions
uint64_t memchr_search(const search_params_t *params,
                       const char *text_start,
                       size_t text_len,
                       match_result_t *result)
{
    // --- Add max_count == 0 check ---
    if (params->max_count == 0)
        return 0;
    // --- End add ---

    uint64_t current_count = 0;             // Use local counter for limit check
    const char target = params->pattern[0]; // Single byte pattern
    const char target_case = params->case_sensitive ? 0 : (islower(target) ? toupper(target) : tolower(target));
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count; // Get max_count

// Special batched buffer for matches to reduce malloc overhead
#define MEMCHR_BUFFER_SIZE 4096
    match_position_t local_buffer[MEMCHR_BUFFER_SIZE];
    size_t buffer_count = 0;

    size_t last_counted_line_start = SIZE_MAX;
    size_t pos = 0;

    // Fast byte-by-byte scan
    while (pos < text_len)
    {
        const char *found;

        // Use platform-optimized memchr
        found = memchr(text_start + pos, target, text_len - pos);

        // Handle case insensitive - we need to check both cases
        if (!params->case_sensitive && !found && target_case != target)
        {
            found = memchr(text_start + pos, target_case, text_len - pos);
        }

        if (!found)
            break;

        // Calculate absolute position
        size_t match_pos = found - text_start;

        // Match found at match_pos
        // Whole word check
        if (params->whole_word && !is_whole_word_match(text_start, text_len, match_pos, match_pos + 1))
        {
            pos = match_pos + 1;
            continue;
        }

        if (count_lines_mode)
        {
            size_t line_start = find_line_start(text_start, text_len, match_pos);
            if (line_start != last_counted_line_start)
            {
                // --- Check max_count BEFORE incrementing ---
                if (max_count != SIZE_MAX && current_count >= max_count)
                {
                    break; // Limit reached
                }
                // --- End check ---

                current_count++; // Increment line count
                last_counted_line_start = line_start;

                // Optimize: skip to end of line
                size_t line_end = find_line_end(text_start, text_len, line_start);
                pos = (line_end < text_len) ? line_end + 1 : text_len;
            }
            else
            {
                pos = match_pos + 1;
            }
        }
        else
        {
            // --- Check max_count BEFORE incrementing ---
            if (max_count != SIZE_MAX && current_count >= max_count)
            {
                if (track_positions && result) // Add final match to buffer/result
                {
                    if (buffer_count < MEMCHR_BUFFER_SIZE)
                    {
                        local_buffer[buffer_count].start_offset = match_pos;
                        local_buffer[buffer_count].end_offset = match_pos + 1;
                        buffer_count++;
                    }
                    else if (!match_result_add(result, match_pos, match_pos + 1))
                    {
                        fprintf(stderr, "Warning: Failed to add SSE4.2 match position.\n");
                    }
                }
                break; // Limit reached
            }
            // --- End check ---

            current_count++; // Increment match count

            if (track_positions && result)
            {
                if (buffer_count < MEMCHR_BUFFER_SIZE)
                {
                    // Store in local buffer
                    local_buffer[buffer_count].start_offset = match_pos;
                    local_buffer[buffer_count].end_offset = match_pos + 1;
                    buffer_count++;
                }
                else
                {
                    // Flush buffer to result
                    for (size_t i = 0; i < buffer_count; i++)
                    {
                        match_result_add(result, local_buffer[i].start_offset,
                                         local_buffer[i].end_offset);
                    }
                    buffer_count = 0;
                    // Add current match to buffer
                    local_buffer[buffer_count].start_offset = match_pos;
                    local_buffer[buffer_count].end_offset = match_pos + 1;
                    buffer_count++;
                }
            }
            pos = match_pos + 1;
        }
    }

    // Flush any remaining buffer entries
    if (track_positions && result && buffer_count > 0)
    {
        // Respect max_count when flushing remaining buffer
        uint64_t already_added = result->count;
        uint64_t can_add_more = (max_count == SIZE_MAX) ? buffer_count : ((already_added >= max_count) ? 0 : max_count - already_added);
        size_t flush_limit = (buffer_count < can_add_more) ? buffer_count : can_add_more;

        for (size_t i = 0; i < flush_limit; i++)
        {
            match_result_add(result, local_buffer[i].start_offset,
                             local_buffer[i].end_offset);
        }
    }

    return current_count; // Return line count or match count
}

// --- Thread Pool Implementation ---

// Worker thread function that processes tasks from the queue
static void *thread_pool_worker(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;
    task_t *task;

    while (true)
    {
        // Lock the queue mutex to safely access the task queue
        pthread_mutex_lock(&pool->queue_mutex);

        // Wait for a task or shutdown signal
        while (pool->task_queue == NULL && !atomic_load(&pool->shutdown))
        {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }

        // Check if we should shutdown
        if (atomic_load(&pool->shutdown) && pool->task_queue == NULL)
        {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        // Get a task from the queue
        task = pool->task_queue;
        pool->task_queue = task->next;
        if (pool->task_queue == NULL)
        {
            pool->task_queue_tail = NULL;
        }

        // Increment the working threads counter
        pool->working_threads++;

        // Unlock the queue to allow other threads to get tasks
        pthread_mutex_unlock(&pool->queue_mutex);

        // Execute the task
        if (task != NULL)
        {
            task->func(task->arg);
            free(task);
        }

        // Mark thread as no longer working and signal if all work is done
        pthread_mutex_lock(&pool->queue_mutex);
        pool->working_threads--;
        if (pool->working_threads == 0 && pool->task_queue == NULL)
        {
            pthread_cond_signal(&pool->complete_cond);
        }
        pthread_mutex_unlock(&pool->queue_mutex);
    }

    return NULL;
}

// Initialize a thread pool with the specified number of worker threads
thread_pool_t *thread_pool_init(int num_threads)
{
    if (num_threads <= 0)
    {
        // Auto-detect core count if not specified
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_threads <= 0)
        {
            num_threads = 4; // Fallback to a reasonable default
        }
        // Use slightly fewer threads than cores to leave headroom
        if (num_threads > 2)
            num_threads = num_threads - 1;
    }

    thread_pool_t *pool = malloc(sizeof(thread_pool_t));
    if (!pool)
    {
        return NULL;
    }

    // Initialize pool structure
    pool->threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool->threads)
    {
        free(pool);
        return NULL;
    }

    pool->num_threads = num_threads;
    pool->task_queue = NULL;
    pool->task_queue_tail = NULL;
    pool->working_threads = 0;
    atomic_init(&pool->shutdown, false);

    // Initialize mutex with PTHREAD_MUTEX_ADAPTIVE_NP for better spin behavior
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
#ifdef PTHREAD_MUTEX_ADAPTIVE_NP
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
    if (pthread_mutex_init(&pool->queue_mutex, &mutex_attr) != 0)
    {
        pthread_mutexattr_destroy(&mutex_attr);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pthread_mutexattr_destroy(&mutex_attr);

    if (pthread_cond_init(&pool->queue_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&pool->queue_mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    if (pthread_cond_init(&pool->complete_cond, NULL) != 0)
    {
        pthread_cond_destroy(&pool->queue_cond);
        pthread_mutex_destroy(&pool->queue_mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    // Set thread attributes for better performance
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    
    // Set a reasonable stack size (256KB should be enough for search operations)
    pthread_attr_setstacksize(&thread_attr, 256 * 1024);

    // Create worker threads
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], &thread_attr, thread_pool_worker, pool) != 0)
        {
            // Handle failure - stop and clean up
            atomic_store(&pool->shutdown, true);
            pthread_cond_broadcast(&pool->queue_cond);

            // Wait for any started threads and clean up
            for (int j = 0; j < i; j++)
            {
                pthread_join(pool->threads[j], NULL);
            }

            pthread_attr_destroy(&thread_attr);
            pthread_cond_destroy(&pool->complete_cond);
            pthread_cond_destroy(&pool->queue_cond);
            pthread_mutex_destroy(&pool->queue_mutex);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    
    pthread_attr_destroy(&thread_attr);

    return pool;
}

// Submit a task to the thread pool
bool thread_pool_submit(thread_pool_t *pool, void *(*func)(void *), void *arg)
{
    if (UNLIKELY(!pool || !func || atomic_load(&pool->shutdown)))
    {
        return false;
    }

    // Create a new task
    task_t *task = malloc(sizeof(task_t));
    if (!task)
    {
        return false;
    }

    task->func = func;
    task->arg = arg;
    task->next = NULL;

    // Add task to queue
    pthread_mutex_lock(&pool->queue_mutex);

    if (pool->task_queue == NULL)
    {
        // Queue was empty
        pool->task_queue = task;
        pool->task_queue_tail = task;
    }
    else
    {
        // Append to end of queue
        pool->task_queue_tail->next = task;
        pool->task_queue_tail = task;
    }

    // Signal that work is available
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    return true;
}

// Submit multiple tasks at once for better efficiency
bool thread_pool_submit_batch(thread_pool_t *pool, void *(*func)(void *), void **args, int count)
{
    if (UNLIKELY(!pool || !func || !args || count <= 0 || atomic_load(&pool->shutdown)))
    {
        return false;
    }

    // Pre-allocate all tasks
    task_t *tasks = malloc(count * sizeof(task_t));
    if (!tasks)
    {
        return false;
    }

    // Initialize tasks
    for (int i = 0; i < count; i++)
    {
        tasks[i].func = func;
        tasks[i].arg = args[i];
        tasks[i].next = (i < count - 1) ? &tasks[i + 1] : NULL;
    }

    // Add all tasks to queue at once
    pthread_mutex_lock(&pool->queue_mutex);

    if (pool->task_queue == NULL)
    {
        pool->task_queue = &tasks[0];
    }
    else
    {
        pool->task_queue_tail->next = &tasks[0];
    }
    pool->task_queue_tail = &tasks[count - 1];

    // Broadcast to wake all waiting threads
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    return true;
}

// Wait for all tasks to complete
void thread_pool_wait_all(thread_pool_t *pool)
{
    if (!pool)
    {
        return;
    }

    pthread_mutex_lock(&pool->queue_mutex);

    // Wait until the task queue is empty and all threads are idle
    while (pool->task_queue != NULL || pool->working_threads > 0)
    {
        pthread_cond_wait(&pool->complete_cond, &pool->queue_mutex);
    }

    pthread_mutex_unlock(&pool->queue_mutex);
}

// Destroy the thread pool
void thread_pool_destroy(thread_pool_t *pool)
{
    if (!pool)
    {
        return;
    }

    // Set the shutdown flag to true
    atomic_store(&pool->shutdown, true);

    // Wake up all worker threads
    pthread_mutex_lock(&pool->queue_mutex);
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    // Wait for all threads to finish
    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // Clean up any remaining tasks (should be none if wait_all was called)
    task_t *task = pool->task_queue;
    while (task != NULL)
    {
        task_t *next = task->next;
        free(task);
        task = next;
    }

    // Clean up resources
    pthread_cond_destroy(&pool->complete_cond);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_mutex_destroy(&pool->queue_mutex);
    free(pool->threads);
    free(pool);
}

// --- memchr-based search for short patterns (2-3 chars) ---
uint64_t memchr_short_search(const search_params_t *params,
                             const char *text_start,
                             size_t text_len,
                             match_result_t *result)
{
    if (params->max_count == 0 && (params->count_lines_mode || params->track_positions))
        return 0;

    uint64_t current_count = 0;
    size_t pattern_len = params->pattern_len;
    const unsigned char *search_pattern = (const unsigned char *)params->pattern;
    bool case_sensitive = params->case_sensitive;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count;

    if (pattern_len < 2 || pattern_len > 3 || text_len < pattern_len)
        return 0;

    const char *current_pos = text_start;
    size_t remaining_len = text_len;
    size_t last_counted_line_start = SIZE_MAX;
    unsigned char first_char = search_pattern[0];
    unsigned char first_char_lower = case_sensitive ? 0 : lower_table[first_char];

    while (remaining_len >= pattern_len)
    {
        const char *potential_match = NULL;
        if (case_sensitive)
        {
            potential_match = memchr(current_pos, first_char, remaining_len - pattern_len + 1);
        }
        else
        {
            const unsigned char *scan = (const unsigned char *)current_pos;
            for (size_t k = 0; k <= remaining_len - pattern_len; ++k)
            {
                if (lower_table[scan[k]] == first_char_lower)
                {
                    potential_match = (const char *)(scan + k);
                    break;
                }
            }
        }

        if (potential_match == NULL)
        {
            break;
        }

        bool full_match = false;
        if (case_sensitive)
        {
            if (memcmp(potential_match + 1, search_pattern + 1, pattern_len - 1) == 0)
            {
                full_match = true;
            }
        }
        else
        {
            if (memory_equals_case_insensitive((const unsigned char *)potential_match + 1, search_pattern + 1, pattern_len - 1))
            {
                full_match = true;
            }
        }

        if (full_match)
        {
            size_t match_start_offset = potential_match - text_start;
            // Whole word check
            if (params->whole_word && !is_whole_word_match(text_start, text_len, match_start_offset, match_start_offset + pattern_len))
            {
                remaining_len -= (potential_match - current_pos) + 1;
                current_pos = potential_match + 1;
                continue;
            }

            bool count_incremented_this_match = false;

            if (count_lines_mode)
            {
                size_t line_start = find_line_start(text_start, text_len, match_start_offset);
                if (line_start != last_counted_line_start)
                {
                    current_count++;
                    last_counted_line_start = line_start;
                    count_incremented_this_match = true;
                }
            }
            else
            {
                current_count++;
                count_incremented_this_match = true;
                if (track_positions && result)
                {
                    if (current_count <= max_count)
                    {
                        if (!match_result_add(result, match_start_offset, match_start_offset + pattern_len))
                        {
                            fprintf(stderr, "Warning: Failed to add short match position.\n");
                        }
                    }
                }
            }

            if (count_incremented_this_match && current_count >= max_count)
            {
                break;
            }
        }

        size_t advance = (potential_match - current_pos) + (only_matching ? pattern_len : 1);
        if (advance > remaining_len)
            break;
        current_pos += advance;
        remaining_len -= advance;
    }

    return current_count;
}

#ifdef __ARM_NEON
uint64_t neon_search(const search_params_t *params,
                     const char *text_start,
                     size_t text_len,
                     match_result_t *result)
{
    // Precondition checks
    if (params->pattern_len == 0 || !params->case_sensitive || text_len < params->pattern_len)
    {
        return boyer_moore_search(params, text_start, text_len, result);
    }
    if (params->max_count == 0 && (params->count_lines_mode || params->track_positions))
        return 0;

    uint64_t current_count = 0;
    size_t pattern_len = params->pattern_len;
    const char *pattern = params->pattern;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count;
    size_t last_counted_line_start = SIZE_MAX;

    // Broadcast the first character of the pattern to a 128-bit vector
    uint8x16_t first_char_vec = vdupq_n_u8((uint8_t)pattern[0]);

    const char *current_pos = text_start;
    size_t remaining_len = text_len;

    // Process in 16-byte chunks
    while (remaining_len >= 16)
    {
        // Load 16 bytes of text
        uint8x16_t text_vec = vld1q_u8((const uint8_t *)current_pos);

        // Compare with first character
        uint8x16_t cmp = vceqq_u8(text_vec, first_char_vec);

        // Check if any match found
        // vmaxvq_u8 returns the maximum value across the vector. If any byte matched (0xFF), result is 0xFF.
        if (vmaxvq_u8(cmp) != 0)
        {
            // Extract mask to find exact positions
            // Since NEON doesn't have a direct movemask, we simulate it or iterate.
            // For simplicity and portability, we can store the comparison result to a temporary array
            // or use a narrowing shift trick to create a 64-bit mask.
            
            // Efficient mask extraction for NEON:
            uint8_t match_mask[16] __attribute__((aligned(16)));
            vst1q_u8(match_mask, cmp);

            for (int i = 0; i < 16; ++i)
            {
                if (match_mask[i] != 0)
                {
                    // Potential match at offset i
                    // Check bounds for full pattern match
                    if (remaining_len - i < pattern_len) 
                        continue; // Should be handled by outer loop condition mostly, but safety first

                    // Verify full pattern match
                    if (memcmp(current_pos + i, pattern, pattern_len) == 0)
                    {
                        size_t match_start_offset = (current_pos - text_start) + i;

                        // Whole word check
                        if (params->whole_word && !is_whole_word_match(text_start, text_len, match_start_offset, match_start_offset + pattern_len))
                        {
                            continue;
                        }

                        bool count_incremented_this_match = false;

                        if (count_lines_mode)
                        {
                            size_t line_start = find_line_start(text_start, text_len, match_start_offset);
                            if (line_start != last_counted_line_start)
                            {
                                if (current_count >= max_count) goto end_neon_search;

                                current_count++;
                                last_counted_line_start = line_start;
                                count_incremented_this_match = true;

                                // Optimization: skip to end of line
                                size_t line_end = find_line_end(text_start, text_len, line_start);
                                if (line_end < text_len)
                                {
                                    // Calculate how much to advance current_pos to reach the start of the next line
                                    // current_pos is the start of the current 16-byte chunk
                                    // We want current_pos to become (text_start + line_end + 1)
                                    
                                    size_t next_line_start = line_end + 1;
                                    size_t current_pos_offset = current_pos - text_start;
                                    
                                    // Ensure we are advancing forward
                                    if (next_line_start > current_pos_offset)
                                    {
                                        size_t advance = next_line_start - current_pos_offset;
                                        
                                        // Ensure we don't advance past end
                                        if (advance > remaining_len) advance = remaining_len;
                                        
                                        current_pos += advance;
                                        remaining_len -= advance;
                                        goto next_chunk; // Jump to next iteration of outer loop
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (current_count >= max_count) goto end_neon_search;

                            current_count++;
                            count_incremented_this_match = true;

                            if (track_positions && result)
                            {
                                if (current_count <= max_count)
                                {
                                    if (!match_result_add(result, match_start_offset, match_start_offset + pattern_len))
                                    {
                                        fprintf(stderr, "Warning: Failed to add NEON match position.\n");
                                    }
                                }
                            }
                        }

                        if (count_incremented_this_match && current_count >= max_count)
                        {
                            goto end_neon_search;
                        }
                        
                        // If only_matching is false (finding all occurrences including overlaps not typically required for simple search unless specified)
                        // But standard grep doesn't usually overlap. 
                        // If we found a match, we can potentially skip pattern_len - 1 to avoid re-checking.
                        // However, for simplicity and correctness with the loop i++, we just continue.
                        // Optimizing this would require jumping 'i'.
                    }
                }
            }
        }
        
        // Advance to next chunk
        current_pos += 16;
        remaining_len -= 16;
        
    next_chunk:;
    }

    // Handle tail
    if (remaining_len >= pattern_len)
    {
        search_params_t tail_params = *params;
        if (max_count != SIZE_MAX)
        {
            tail_params.max_count = (current_count >= max_count) ? 0 : max_count - current_count;
        }
        
        uint64_t tail_count = boyer_moore_search(&tail_params, current_pos, remaining_len, result);
        
        // Fixup result offsets if needed (boyer_moore adds relative to current_pos if we passed result)
        // Actually boyer_moore adds absolute offsets if we pass text_start as current_pos? 
        // No, boyer_moore takes "text_start" argument. If we pass `current_pos`, it treats that as 0.
        // So we need to adjust if we passed `result`.
        
        if (result && track_positions && tail_count > 0)
        {
             // Fix offsets for the tail matches
             size_t tail_offset = current_pos - text_start;
             // The matches were added at the end of result->positions
             // We need to find which ones were just added.
             // This is tricky if we don't know exactly how many were added, but we do: tail_count.
             // Wait, boyer_moore returns the count.
             
             size_t start_idx = result->count - tail_count;
             if (result->count >= tail_count) { // Safety check
                 for(size_t k = 0; k < tail_count; ++k) {
                     result->positions[start_idx + k].start_offset += tail_offset;
                     result->positions[start_idx + k].end_offset += tail_offset;
                 }
             }
        }
        
        current_count += tail_count;
    }

end_neon_search:
    return current_count;
}
#endif

// --- SIMD Implementations (Placeholders/Actual) ---

#if KREP_USE_SSE42
// SSE4.2 search function using _mm_cmpestri
// Handles case-sensitive patterns up to 16 bytes.
uint64_t simd_sse42_search(const search_params_t *params,
                           const char *text_start,
                           size_t text_len,
                           match_result_t *result)
{
    // Precondition checks
    if (params->pattern_len == 0 || params->pattern_len > 16 || !params->case_sensitive || text_len < params->pattern_len)
    {
        // Fallback if preconditions not met
        return boyer_moore_search(params, text_start, text_len, result);
    }
    if (params->max_count == 0 && (params->count_lines_mode || params->track_positions))
        return 0;

    uint64_t current_count = 0;
    size_t pattern_len = params->pattern_len;
    const char *pattern = params->pattern;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count;
    size_t last_counted_line_start = SIZE_MAX;

    // Load the pattern into an XMM register
    __m128i pattern_vec = _mm_loadu_si128((const __m128i *)pattern);

    const char *current_pos = text_start;
    size_t remaining_len = text_len;

    // Mode for _mm_cmpestri: compare strings, return index, positive polarity
    // Using an enum for constant folding to work with optimizations off
    enum
    {
        cmp_mode = _SIDD_CMP_EQUAL_ORDERED | _SIDD_POSITIVE_POLARITY | _SIDD_LEAST_SIGNIFICANT
    };

    while (remaining_len >= pattern_len)
    {
        // Determine chunk size (max 16 bytes for _mm_cmpestri)
        size_t chunk_len = (remaining_len < 16) ? remaining_len : 16;

        // Load text chunk into an XMM register - SAFELY
        __m128i text_vec;
        if (chunk_len < 16)
        {
            // For smaller chunks, use a buffer to avoid reading past the end
            char safe_buffer[16] = {0}; // Zero-initialized
            memcpy(safe_buffer, current_pos, chunk_len);
            text_vec = _mm_loadu_si128((const __m128i *)safe_buffer);
        }
        else
        {
            text_vec = _mm_loadu_si128((const __m128i *)current_pos);
        }

        // Compare pattern against the text chunk
        // _mm_cmpestri returns the index of the first byte of the first match
        // or chunk_len if no match is found within the chunk.
        int index = _mm_cmpestri(pattern_vec, pattern_len, text_vec, chunk_len, cmp_mode);

        if (index < (int)(chunk_len - pattern_len + 1))
        {
            // Match found within the current 16-byte window at 'index'
            size_t match_start_offset = (current_pos - text_start) + index;

            // Fast path: skip whole word check if not needed
            if (!params->whole_word || is_whole_word_match(text_start, text_len, match_start_offset, match_start_offset + pattern_len))
            {
                bool count_incremented_this_match = false;

                if (count_lines_mode)
                {
                    // Cache the line start position to avoid repeated calculations
                    size_t line_start = find_line_start(text_start, text_len, match_start_offset);
                    if (line_start != last_counted_line_start)
                    {
                        // Check max count before incrementing
                        if (current_count >= max_count)
                            break;

                        current_count++;
                        last_counted_line_start = line_start;
                        count_incremented_this_match = true;

                        // Optimize: advance to next line after counting this one
                        size_t line_end = find_line_end(text_start, text_len, line_start);
                        if (line_end < text_len)
                        {
                            // Convert to offsets from text_start to fix pointer arithmetic
                            size_t current_offset = (current_pos - text_start) + index;
                            size_t advance = (line_end + 1) - current_offset;
                            if (advance > 0)
                            {
                                current_pos += advance;
                                remaining_len -= advance;
                                continue;
                            }
                        }
                    }
                }
                else
                {
                    // Check max count before incrementing
                    if (current_count >= max_count)
                        break;

                    current_count++;
                    count_incremented_this_match = true;

                    if (track_positions && result)
                    {
                        // Add position only if still within max_count
                        if (current_count <= max_count)
                        {
                            // Minimize error checking in tight loop for better performance
                            if (result->count < result->capacity)
                            {
                                result->positions[result->count].start_offset = match_start_offset;
                                result->positions[result->count].end_offset = match_start_offset + pattern_len;
                                result->count++;
                            }
                            else if (!match_result_add(result, match_start_offset, match_start_offset + pattern_len))
                            {
                                fprintf(stderr, "Warning: Failed to add SSE4.2 match position.\n");
                            }
                        }
                    }
                }

                // Early break if max count reached
                if (count_incremented_this_match && current_count >= max_count)
                {
                    break;
                }
            }

            // More aggressive advancement strategy
            // Advance to just after the match instead of just by one byte
            size_t advance = index + 1;

            // If not looking for overlapping matches, can advance by pattern length
            if (!only_matching)
            { // only_matching mode needs to find overlapping matches
                advance = index + pattern_len;
                // Ensure we don't advance too far if near end
                if (advance > remaining_len)
                    advance = remaining_len;
            }

            current_pos += advance;
            remaining_len -= advance;
        }
        else
        {
            // No match found in this chunk. Advance more aggressively.
            // Jump by almost the full chunk size, leaving just enough overlap
            // for potential matches that span chunk boundaries.
            size_t advance = chunk_len > pattern_len ? chunk_len - pattern_len + 1 : 1;
            // Ensure we don't advance beyond text bounds
            if (advance > remaining_len)
                advance = remaining_len;

            current_pos += advance;
            remaining_len -= advance;
        }
    }

    return current_count;
}
#endif

#if KREP_USE_AVX2
// AVX2 search function
// Handles case-sensitive patterns up to 32 bytes.
// Uses SSE4.2 logic for patterns <= 16 bytes.
// Uses a simplified first/last byte check for patterns > 16 bytes.
uint64_t simd_avx2_search(const search_params_t *params,
                          const char *text_start,
                          size_t text_len,
                          match_result_t *result)
{
    // Precondition checks
    if (params->pattern_len == 0 || params->pattern_len > 32 || !params->case_sensitive || text_len < params->pattern_len)
    {
        return boyer_moore_search(params, text_start, text_len, result);
    }
    if (params->max_count == 0 && (params->count_lines_mode || params->track_positions))
        return 0;

    // Use SSE4.2 logic if pattern fits and SSE4.2 is available
#if KREP_USE_SSE42
    if (params->pattern_len <= 16)
    {
        return simd_sse42_search(params, text_start, text_len, result);
    }
#endif

    // --- AVX2 specific logic for pattern_len > 16 and <= 32 ---
    uint64_t current_count = 0;
    size_t pattern_len = params->pattern_len;
    const char *pattern = params->pattern;
    bool count_lines_mode = params->count_lines_mode;
    bool track_positions = params->track_positions;
    size_t max_count = params->max_count;
    size_t last_counted_line_start = SIZE_MAX;

    // Create vectors for the first and last bytes of the pattern
    __m256i first_byte_vec = _mm256_set1_epi8(pattern[0]);
    __m256i last_byte_vec = _mm256_set1_epi8(pattern[pattern_len - 1]);

    const char *current_pos = text_start;
    size_t remaining_len = text_len;

    while (remaining_len >= 32) // Process in 32-byte chunks
    {
        // Prefetch next cache lines for better memory performance
        if (LIKELY(remaining_len > PREFETCH_DISTANCE))
            __builtin_prefetch(current_pos + PREFETCH_DISTANCE, 0, 0);

        // Load 32 bytes of text - SAFELY
        __m256i text_vec;
        if (remaining_len < 32)
        {
            // For smaller chunks, use a buffer to avoid reading past the end
            char safe_buffer[32] = {0}; // Zero-initialized
            memcpy(safe_buffer, current_pos, remaining_len);
            text_vec = _mm256_loadu_si256((const __m256i *)safe_buffer);
        }
        else
        {
            text_vec = _mm256_loadu_si256((const __m256i *)current_pos);
        }

        // Compare first byte of pattern with text
        __m256i first_cmp = _mm256_cmpeq_epi8(first_byte_vec, text_vec);

        // Compare last byte of pattern with text shifted by pattern_len - 1
        // This requires loading potentially unaligned data for the last byte comparison
        // SAFELY handle this load too
        __m256i text_last_byte_vec;
        size_t last_byte_offset = pattern_len - 1;
        size_t bytes_available = remaining_len > last_byte_offset ? remaining_len - last_byte_offset : 0;

        if (bytes_available < 32)
        {
            char safe_buffer[32] = {0}; // Zero-initialized
            size_t copy_size = bytes_available < remaining_len ? bytes_available : remaining_len;
            memcpy(safe_buffer, current_pos + last_byte_offset, copy_size);
            text_last_byte_vec = _mm256_loadu_si256((const __m256i *)safe_buffer);
        }
        else
        {
            text_last_byte_vec = _mm256_loadu_si256((const __m256i *)(current_pos + last_byte_offset));
        }
        __m256i last_cmp = _mm256_cmpeq_epi8(last_byte_vec, text_last_byte_vec);

        // Combine the masks: a potential match starts where both first and last bytes match
        // Note: _mm256_and_si256 operates on the comparison results directly
        // We need the mask of indices where *both* comparisons are true.
        // Get integer masks
        uint32_t first_mask = _mm256_movemask_epi8(first_cmp);
        // The last_mask needs to correspond to the *start* position of the potential match
        uint32_t last_mask = _mm256_movemask_epi8(last_cmp);

        // Combine masks: potential match starts at index 'i' if bit 'i' is set in both masks.
        uint32_t potential_starts_mask = first_mask & last_mask;

        // Iterate through potential start positions indicated by the combined mask
        while (potential_starts_mask != 0)
        {
            // Find the index of the lowest set bit (potential match start)
            int index = __builtin_ctz(potential_starts_mask); // Use compiler intrinsic for count trailing zeros

            // Verify the full pattern match at this position
            if (memcmp(current_pos + index, pattern, pattern_len) == 0)
            {
                // Full match confirmed
                size_t match_start_offset = (current_pos - text_start) + index;
                // Whole word check
                if (params->whole_word && !is_whole_word_match(text_start, text_len, match_start_offset, match_start_offset + pattern_len))
                {
                    potential_starts_mask &= potential_starts_mask - 1;
                    continue;
                }

                bool count_incremented_this_match = false;

                if (count_lines_mode)
                {
                    size_t line_start = find_line_start(text_start, text_len, match_start_offset);
                    if (line_start != last_counted_line_start)
                    {
                        current_count++;
                        last_counted_line_start = line_start;
                        count_incremented_this_match = true;
                    }
                }
                else
                {
                    current_count++;
                    count_incremented_this_match = true;
                    if (track_positions && result)
                    {
                        if (current_count <= max_count)
                        {
                            if (!match_result_add(result, match_start_offset, match_start_offset + pattern_len))
                            {
                                fprintf(stderr, "Warning: Failed to add AVX2 match position.\n");
                            }
                        }
                    }
                }

                // Check max_count limit
                if (count_incremented_this_match && current_count >= max_count)
                {
                    goto end_avx2_search; // Exit outer loop
                }
            }

            // Clear the found bit to find the next potential start
            potential_starts_mask &= potential_starts_mask - 1;
        }

        // Advance position. Advance by 32 for simplicity, might miss overlaps near boundary.
        // A safer advance would be smaller, e.g., 1 or based on last potential match.
        // For this basic version, we advance by 32.
        current_pos += 32;
        remaining_len -= 32;
    }

    // Handle the remaining tail (less than 32 bytes) using scalar search
    if (remaining_len >= pattern_len)
    {
        // Create a temporary params struct for the tail search
        search_params_t tail_params = *params;
        // Adjust max_count for the remaining part
        if (max_count != SIZE_MAX)
        {
            tail_params.max_count = (current_count >= max_count) ? 0 : max_count - current_count;
        }

        // Use Boyer-Moore for the tail
        uint64_t tail_count = boyer_moore_search(&tail_params, current_pos, remaining_len, result);

        // Adjust global count and potentially merge results (BM adds directly if result is passed)
        // Need to adjust offsets if result was passed to BM
        if (result && track_positions && tail_count > 0)
        {
            // Find where the tail results start in the global result list
            uint64_t bm_start_index = current_count; // Assuming BM added sequentially
            if (bm_start_index > result->count)
                bm_start_index = result->count; // Safety check
            uint64_t added_by_bm = result->count - bm_start_index;

            size_t tail_offset = current_pos - text_start;
            for (uint64_t k = 0; k < added_by_bm; ++k)
            {
                result->positions[bm_start_index + k].start_offset += tail_offset;
                result->positions[bm_start_index + k].end_offset += tail_offset;
            }
        }
        current_count += tail_count;
        // Ensure final count doesn't exceed max_count
        if (max_count != SIZE_MAX && current_count > max_count)
        {
            current_count = max_count;
            // Note: Result list might have slightly more entries than max_count here,
            // but print_matching_items respects max_count.
        }
    }

end_avx2_search:
    return current_count;
}
#endif

#if KREP_USE_AVX512
// AVX-512 search function - Ultra high-performance search for 64-byte patterns
// Uses 512-bit registers for maximum throughput on supported hardware
HOT_FUNCTION
uint64_t simd_avx512_search(const search_params_t *params,
                            const char *text_start,
                            size_t text_len,
                            match_result_t *result)
{
    // Precondition checks
    if (UNLIKELY(params->pattern_len == 0 || params->pattern_len > 64 || 
                 !params->case_sensitive || text_len < params->pattern_len))
    {
        return simd_avx2_search(params, text_start, text_len, result);
    }
    if (UNLIKELY(params->max_count == 0 && (params->count_lines_mode || params->track_positions)))
        return 0;

    // Use AVX2 for smaller patterns (more efficient)
    if (params->pattern_len <= 32)
    {
        return simd_avx2_search(params, text_start, text_len, result);
    }

    // --- AVX-512 specific logic for pattern_len > 32 and <= 64 ---
    uint64_t current_count = 0;
    const size_t pattern_len = params->pattern_len;
    const char *pattern = params->pattern;
    const bool count_lines_mode = params->count_lines_mode;
    const bool track_positions = params->track_positions;
    const size_t max_count = params->max_count;
    size_t last_counted_line_start = SIZE_MAX;

    // Create 512-bit vectors for the first and last bytes of the pattern
    const __m512i first_byte_vec = _mm512_set1_epi8(pattern[0]);
    const __m512i last_byte_vec = _mm512_set1_epi8(pattern[pattern_len - 1]);

    const char *current_pos = text_start;
    size_t remaining_len = text_len;

    // Process in 64-byte chunks (512 bits)
    while (remaining_len >= 64)
    {
        // Aggressive prefetching for streaming access
        if (LIKELY(remaining_len > PREFETCH_DISTANCE * 2))
        {
            __builtin_prefetch(current_pos + PREFETCH_DISTANCE, 0, 0);
            __builtin_prefetch(current_pos + PREFETCH_DISTANCE * 2, 0, 0);
        }

        // Load 64 bytes of text
        __m512i text_vec = _mm512_loadu_si512((const __m512i *)current_pos);

        // Compare first byte of pattern with text
        __mmask64 first_mask = _mm512_cmpeq_epi8_mask(first_byte_vec, text_vec);

        // Quick exit if no first byte matches
        if (first_mask == 0)
        {
            current_pos += 64;
            remaining_len -= 64;
            continue;
        }

        // Load last byte positions (offset by pattern_len - 1)
        size_t last_byte_offset = pattern_len - 1;
        if (remaining_len >= last_byte_offset + 64)
        {
            __m512i text_last_byte_vec = _mm512_loadu_si512((const __m512i *)(current_pos + last_byte_offset));
            __mmask64 last_mask = _mm512_cmpeq_epi8_mask(last_byte_vec, text_last_byte_vec);

            // Combine masks: potential match starts where both first and last bytes match
            uint64_t potential_starts_mask = first_mask & last_mask;

            // Process all potential matches
            while (potential_starts_mask != 0)
            {
                // Find the index of the lowest set bit
                int index = __builtin_ctzll(potential_starts_mask);

                // Verify full pattern match
                if (memcmp(current_pos + index, pattern, pattern_len) == 0)
                {
                    size_t match_start_offset = (current_pos - text_start) + index;

                    // Whole word check
                    if (params->whole_word && 
                        !is_whole_word_match(text_start, text_len, match_start_offset, match_start_offset + pattern_len))
                    {
                        potential_starts_mask &= potential_starts_mask - 1;
                        continue;
                    }

                    bool count_incremented = false;

                    if (count_lines_mode)
                    {
                        size_t line_start = find_line_start(text_start, text_len, match_start_offset);
                        if (line_start != last_counted_line_start)
                        {
                            current_count++;
                            last_counted_line_start = line_start;
                            count_incremented = true;
                        }
                    }
                    else
                    {
                        current_count++;
                        count_incremented = true;
                        if (track_positions && result && current_count <= max_count)
                        {
                            match_result_add(result, match_start_offset, match_start_offset + pattern_len);
                        }
                    }

                    if (count_incremented && current_count >= max_count)
                    {
                        return current_count;
                    }
                }

                potential_starts_mask &= potential_starts_mask - 1;
            }
        }

        current_pos += 64;
        remaining_len -= 64;
    }

    // Handle tail with AVX2
    if (remaining_len >= pattern_len)
    {
        search_params_t tail_params = *params;
        if (max_count != SIZE_MAX)
        {
            tail_params.max_count = (current_count >= max_count) ? 0 : max_count - current_count;
        }

        uint64_t tail_count = simd_avx2_search(&tail_params, current_pos, remaining_len, result);

        // Fix offsets for tail matches
        if (result && track_positions && tail_count > 0)
        {
            size_t tail_offset = current_pos - text_start;
            uint64_t start_idx = result->count >= tail_count ? result->count - tail_count : 0;
            for (uint64_t k = 0; k < tail_count && (start_idx + k) < result->count; ++k)
            {
                result->positions[start_idx + k].start_offset += tail_offset;
                result->positions[start_idx + k].end_offset += tail_offset;
            }
        }

        current_count += tail_count;
    }

    return current_count;
}
#endif
