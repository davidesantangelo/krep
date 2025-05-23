/**
 * Test header for krep string search utility
 */

#ifndef TEST_KREP_H
#define TEST_KREP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h> // For SIZE_MAX
#include <regex.h>  // Include for regex_t

/* Define TESTING to enable the compatibility wrappers if not already defined by Makefile */
#ifndef TESTING
#define TESTING
#endif

/* Include the main header AFTER defining TESTING potentially */
#include "../krep.h"

/*
 * Note: We use compatibility wrappers defined elsewhere (test_compat.h or krep.c)
 * for most search functions when TESTING is defined.
 * However, we might need the correct declaration for functions called directly
 * or if wrappers aren't used for everything.
 */

/**
 * Test-specific function with a different signature from production code.
 * Used for specific test cases that need to track position data.
 */
uint64_t regex_search_test_positions(const char *text_start, size_t text_len, const regex_t *compiled_regex,
                                     size_t report_limit_offset, bool count_lines_mode,
                                     uint64_t *line_match_count, size_t *last_counted_line_start, size_t *last_matched_line_end,
                                     bool track_positions, match_result_t *result);

/* Declarations for regex test functions */
void test_basic_regex(void);
void test_complex_regex(void);
void test_regex_multiple_matches(void);
void test_regex_edge_cases(void);
void test_regex_overlapping(void);
void test_regex_report_limit(void);
void test_regex_vs_literal_performance(void);
void test_regex_line_extraction(void);
void run_regex_tests(void);

/* Helper function declarations */

/**
 * @brief Creates search parameters for a single literal pattern test.
 *
 * Allocates memory for patterns and pattern_lens arrays. Caller must call cleanup_params.
 *
 * @param pattern The literal string pattern.
 * @param case_sensitive True for case-sensitive search.
 * @param count_lines True if simulating -c mode.
 * @param only_match True if simulating -o mode (influences track_positions).
 * @return A search_params_t structure configured for the test.
 */
search_params_t create_literal_params(const char *pattern, bool case_sensitive, bool count_lines, bool only_match);

/**
 * @brief Creates search parameters for a single regex pattern test.
 *
 * Allocates memory for patterns, pattern_lens, and compiles the regex. Caller must call cleanup_params.
 *
 * @param pattern The regex string pattern.
 * @param case_sensitive True for case-sensitive search.
 * @param count_lines True if simulating -c mode.
 * @param only_match True if simulating -o mode (influences track_positions).
 * @return A search_params_t structure configured for the test.
 */
search_params_t create_regex_params(const char *pattern, bool case_sensitive, bool count_lines, bool only_match);

/**
 * @brief Cleans up resources allocated by create_literal_params or create_regex_params.
 *
 * Frees the compiled regex (if any) and the pattern arrays.
 *
 * @param params Pointer to the search_params_t structure to clean up.
 */
void cleanup_params(search_params_t *params);

#endif /* TEST_KREP_H */