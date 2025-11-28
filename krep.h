/* krep.h - Header file for krep utility
 *
 * Author: Davide Santangelo
 * Year: 2025
 */

#ifndef KREP_H
#define KREP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>     // For size_t definition
#include <regex.h>     // For regex_t type
#include <pthread.h>   // For mutex types (though no longer used for counting)
#include <stdatomic.h> // For atomic types used in structs
#include <ctype.h>     // For isalnum function

// --- Global Variables (declared extern) ---
extern unsigned char lower_table[256];

// Forward declaration for Aho-Corasick trie structure
struct ac_trie;
typedef struct ac_trie ac_trie_t;

/* --- Compiler-specific macros --- */
#ifdef __GNUC__
#define KREP_UNUSED __attribute__((unused))
#else
#define KREP_UNUSED
#endif

/* --- ANSI Color Codes --- */
// Define colors consistently
#define KREP_COLOR_RESET "\033[0m"
#define KREP_COLOR_FILENAME "\033[1;35m"  // Magenta
#define KREP_COLOR_SEPARATOR "\033[1;90m" // Bright Black (Gray)
#define KREP_COLOR_MATCH "\033[1;31m"     // Bright Red
#define KREP_COLOR_TEXT "\033[0m"         // Default terminal text color

/* --- Match tracking structure --- */
// Define match_position_t BEFORE match_result_t
typedef struct
{
   size_t start_offset; // Starting position of the match
   size_t end_offset;   // Ending position of the match (exclusive)
} match_position_t;

typedef struct match_result_t // Add the struct tag here
{
   match_position_t *positions; // Dynamically resizing array of match positions
   uint64_t count;              // Number of matches found and stored
   uint64_t capacity;           // Current allocated capacity of the positions array
} match_result_t;               // Typedef remains the same

/* --- Structures for Multithreading --- */

// Parameters shared across search functions and threads
typedef struct search_params
{
   // Single pattern legacy fields (still used by some functions)
   const char *pattern;
   size_t pattern_len;

   // Multiple pattern fields
   const char **patterns; // Array of patterns (for multiple -e options)
   size_t *pattern_lens;  // Array of pattern lengths
   size_t num_patterns;   // Number of patterns

   // Search options
   bool case_sensitive;
   bool use_regex;
   bool count_lines_mode;   // True for -c mode (count lines)
   bool count_matches_mode; // True for -co mode (count matches - internal concept)
   bool track_positions;    // True for default or -o mode (track positions)
   bool whole_word;         // True for -w mode (whole word search)
   // bool overlapping_mode; // Implicitly true for -o, handled by algorithm advance logic

   // Compiled regex (if applicable, compiled once per file/string)
   const regex_t *compiled_regex;

   // Compiled Aho-Corasick trie (if applicable)
   ac_trie_t *ac_trie; // Add pointer for pre-built trie

   // Max count limit from options
   size_t max_count;

} search_params_t;

/* --- Function Pointer Type for Search Algorithms --- */
// Returns count (lines or matches depending on mode). result can be NULL unless track_positions is true.
typedef uint64_t (*search_func_t)(const search_params_t *params,
                                  const char *text_start,
                                  size_t text_len,
                                  match_result_t *result);

// Data passed to each search thread
typedef struct
{
   int thread_id;
   const search_params_t *params; // Pointer to shared search parameters
   const char *chunk_start;       // Pointer to the start of the memory chunk for this thread
   size_t chunk_len;              // Length of the chunk to process (may include overlap)
   search_func_t search_algo;     // Pre-selected search algorithm for this chunk

   // Thread-specific results
   match_result_t *local_result; // For position tracking (default/-o)
   uint64_t count_result;        // For line counting (-c) or match counting (-co)

   // Status flags
   bool error_flag; // Flag to indicate an error occurred in the thread

   // Padding to prevent false sharing (ensure struct size is multiple of 64 bytes)
   // Current size approx 56 bytes. Adding sufficient padding.
   char padding[64]; 
} thread_data_t;

/* --- Thread Pool Implementation --- */
typedef struct task
{
   void *(*func)(void *); // Task function
   void *arg;             // Function argument
   struct task *next;     // Next task in queue
} task_t;

typedef struct
{
   pthread_t *threads;           // Array of worker threads
   int num_threads;              // Number of worker threads
   task_t *task_queue;           // Task queue head
   task_t *task_queue_tail;      // Task queue tail for faster enqueue
   pthread_mutex_t queue_mutex;  // Mutex for task queue access
   pthread_cond_t queue_cond;    // Condition variable for task availability
   pthread_cond_t complete_cond; // Condition variable for task completion
   size_t working_threads;       // Counter for busy threads
   atomic_bool shutdown;         // Shutdown flag
} thread_pool_t;

/* --- Thread Pool API --- */
thread_pool_t *thread_pool_init(int num_threads);
bool thread_pool_submit(thread_pool_t *pool, void *(*func)(void *), void *arg);
void thread_pool_wait_all(thread_pool_t *pool);
void thread_pool_destroy(thread_pool_t *pool);

/* --- Public API --- */

/**
 * @brief Searches a single file for the given pattern(s). Handles multithreading.
 *
 * @param params Search parameters including patterns and options.
 * @param filename Path to the file, or "-" for stdin.
 * @param requested_thread_count Number of threads requested by user (0 for auto).
 * @return 0 if matches found, 1 if no matches found, 2 on error.
 */
int search_file(const search_params_t *params, const char *filename, int requested_thread_count);

/**
 * @brief Searches a string in memory for the given pattern(s) (single-threaded).
 *
 * @param params Search parameters including patterns and options.
 * @param text The string to search within.
 * @return 0 if matches found, 1 if no matches found, 2 on error.
 */
int search_string(const search_params_t *params, const char *text);

/**
 * @brief Recursively searches a directory for the given pattern(s).
 *
 * @param base_dir The starting directory path.
 * @param params Search parameters including patterns and options.
 * @param thread_count Number of threads to use within file searches.
 * @return The total number of errors encountered during the recursive search.
 */
int search_directory_recursive(const char *base_dir, const search_params_t *params, int thread_count);

/* --- Match result management functions --- */
match_result_t *match_result_init(uint64_t initial_capacity);
bool match_result_add(match_result_t *result, size_t start_offset, size_t end_offset);
void match_result_free(match_result_t *result);
bool match_result_merge(match_result_t *dest, const match_result_t *src, size_t chunk_offset);

/**
 * @brief Prints matching lines or parts based on the provided results and parameters.
 *
 * Handles color highlighting, line numbering, and the '-o' (only matching) option.
 *
 * @param filename The name of the file being searched (or NULL for stdin/string).
 * @param text The full text content.
 * @param text_len The length of the text content.
 * @param result A pointer to the match_result_t structure containing match positions.
 * @param params A pointer to the search parameters, including max_count.
 * @return The number of items (lines or matches) printed.
 */
size_t print_matching_items(const char *filename, const char *text, size_t text_len, const match_result_t *result, const search_params_t *params);

// Print usage information
void print_usage(const char *program_name);

/* --- Internal Search Algorithm Declarations (Updated Signature) --- */

uint64_t boyer_moore_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
uint64_t kmp_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
uint64_t regex_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
uint64_t memchr_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
uint64_t memchr_short_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result); // New function for short patterns

// SIMD functions (only declared if supported by compiler flags)
#if defined(__SSE4_2__)
uint64_t simd_sse42_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
#endif

#if defined(__AVX2__)
uint64_t simd_avx2_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
#endif

#if defined(__ARM_NEON)
uint64_t neon_search(const search_params_t *params, const char *text_start, size_t text_len, match_result_t *result);
#endif

/* --- Helper Functions --- */
bool memory_equals_case_insensitive(const unsigned char *s1, const unsigned char *s2, size_t n);
void prepare_bad_char_table(const unsigned char *pattern, size_t pattern_len, int *bad_char_table, bool case_sensitive);
search_func_t select_search_algorithm(const search_params_t *params);
const char *get_algorithm_name(search_func_t func);

/* --- Skip Lists for Recursive Search --- */
// Defined here so they are accessible by search_directory_recursive in krep.c
static const char *skip_directories[] = {
    ".", "..", ".git", "node_modules", ".svn", ".hg", "build", "dist",
    "__pycache__", ".pytest_cache", ".mypy_cache", ".venv", ".env", "venv", "env",
    "target", "bin", "obj"
    // Add more common build/dependency directories if needed
};
static const size_t num_skip_directories = sizeof(skip_directories) / sizeof(skip_directories[0]);

static const char *skip_extensions[] = {
    // Object files, libraries, executables
    ".o", ".so", ".a", ".dll", ".exe", ".lib", ".dylib", ".class", ".pyc", ".pyo", ".obj", ".elf", ".wasm",
    // Archives
    ".zip", ".tar", ".gz", ".bz2", ".xz", ".rar", ".7z", ".jar", ".war", ".ear", ".iso", ".img", ".pkg", ".deb", ".rpm",
    // Images
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tiff", ".webp", ".svg", ".ico", ".psd", ".ai",
    // Audio/Video
    ".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a", ".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv",
    // Documents
    ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",
    // Data/Logs/Backups
    ".dat", ".bin", ".bak", ".log", ".tmp", ".temp",
    // Minified assets (check for .min.js etc. handled separately in should_skip_extension)
    // Editor/System files
    ".swp", ".swo", ".DS_Store",
    // Databases
    ".db", ".sqlite", ".mdb",
    // Fonts
    ".ttf", ".otf", ".woff", ".woff2", ".eot"};
static const size_t num_skip_extensions = sizeof(skip_extensions) / sizeof(skip_extensions[0]);

/* --- Line Finding Functions --- */

/**
 * @brief Find the start of the line containing the given position.
 *
 * @param text Pointer to the text buffer.
 * @param max_len Maximum length of the text buffer.
 * @param pos Position within the text to find the containing line start.
 * @return Position of the start of the line.
 */
size_t find_line_start(const char *text, size_t max_len, size_t pos);

/**
 * @brief Find the end of the line containing the given position.
 *
 * @param text Pointer to the text buffer.
 * @param text_len Length of the text buffer.
 * @param pos Position within the text to find the containing line end.
 * @return Position of the end of the line (index of the newline or text length).
 */
size_t find_line_end(const char *text, size_t text_len, size_t pos);

/* --- Word Character and Whole Word Match Helpers --- */

/**
 * @brief Check if a character is a word character (alnum or _).
 *
 * @param c The character to check.
 * @return True if the character is a word character, false otherwise.
 */
static inline bool is_word_char(char c)
{
   return (isalnum((unsigned char)c) || c == '_');
}

/**
 * @brief Check if a match is a whole word.
 *
 * @param text Pointer to the text buffer.
 * @param text_len Length of the text buffer.
 * @param start Start position of the match.
 * @param end End position of the match.
 * @return True if the match is a whole word, false otherwise.
 */
static inline bool is_whole_word_match(const char *text, size_t text_len, size_t start, size_t end)
{
   if (start > 0 && is_word_char(text[start - 1]))
      return false;
   if (end < text_len && is_word_char(text[end]))
      return false;
   return true;
}

#endif /* KREP_H */