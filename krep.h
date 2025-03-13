/**
 * Header file for krep - A high-performance string search utility
 *
 * Author: Davide Santangelo
 * Version: 0.1.2
 * Year: 2025
 */

#ifndef KREP_H
#define KREP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

int search_file(const char *filename, const char *pattern, size_t pattern_len, bool case_sensitive,
               bool count_only, int thread_count);
int search_string(const char *pattern, size_t pattern_len, const char *text, bool case_sensitive);

/* Search algorithm function declarations */
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

#endif /* KREP_H */
