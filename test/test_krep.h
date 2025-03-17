/**
 * Test header for krep string search utility
 */

#ifndef TEST_KREP_H
#define TEST_KREP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* Function declarations from krep.c that we need for testing */
uint64_t boyer_moore_search(const char *text, size_t text_len,
                          const char *pattern, size_t pattern_len,
                          bool case_sensitive);
uint64_t kmp_search(const char *text, size_t text_len,
                   const char *pattern, size_t pattern_len,
                   bool case_sensitive);
uint64_t rabin_karp_search(const char *text, size_t text_len,
                         const char *pattern, size_t pattern_len,
                         bool case_sensitive);

#ifdef __SSE4_2__
uint64_t simd_search(const char *text, size_t text_len,
                   const char *pattern, size_t pattern_len,
                   bool case_sensitive);
#endif

#ifdef __AVX2__
uint64_t avx2_search(const char *text, size_t text_len,
                   const char *pattern, size_t pattern_len,
                   bool case_sensitive);
#endif

#ifdef __ARM_NEON
uint64_t neon_search(const char *text, size_t text_len,
                    const char *pattern, size_t pattern_len,
                    bool case_sensitive);
#endif

#endif /* TEST_KREP_H */
