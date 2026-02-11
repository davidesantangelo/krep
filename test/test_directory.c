/**
 * Test suite for krep directory search functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

/* Define TESTING before including headers if not done by Makefile */
#ifndef TESTING
#define TESTING
#endif

#include "../krep.h"

// Constants definitions
#define TEST_DIR_BASE "/tmp/krep_test_dir"
#define TEST_FILE_MAX_SIZE 8192
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Function declarations
static void create_test_directory_structure(void);
static void cleanup_test_directory_structure(void);
static void create_binary_file(const char *path);
static void create_text_file(const char *path, const char *content);
static void create_nested_directory(const char *base_path, int depth, int max_depth);
static bool run_recursive_search_capture(const char *base_dir, const search_params_t *params, int thread_count,
                                         char *output, size_t output_size, int *errors_out);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message)      \
    do                                       \
    {                                        \
        if (condition)                       \
        {                                    \
            printf("✓ PASS: %s\n", message); \
            tests_passed++;                  \
        }                                    \
        else                                 \
        {                                    \
            printf("✗ FAIL: %s\n", message); \
            tests_failed++;                  \
        }                                    \
    } while (0)

static bool run_recursive_search_capture(const char *base_dir, const search_params_t *params, int thread_count,
                                         char *output, size_t output_size, int *errors_out)
{
    if (!base_dir || !params || !output || output_size == 0 || !errors_out)
        return false;

    bool ok = false;
    int saved_stdout = -1;
    int out_fd = -1;
    char out_template[] = "/tmp/krep_dir_outputXXXXXX";

    output[0] = '\0';
    *errors_out = -1;

    out_fd = mkstemp(out_template);
    if (out_fd == -1)
    {
        perror("mkstemp failed for directory output capture");
        goto cleanup;
    }

    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout == -1)
    {
        perror("dup failed while capturing stdout");
        goto cleanup;
    }

    fflush(stdout);
    if (dup2(out_fd, STDOUT_FILENO) == -1)
    {
        perror("dup2 failed while redirecting stdout");
        goto cleanup;
    }

    *errors_out = search_directory_recursive(base_dir, params, thread_count);
    fflush(stdout);

    if (dup2(saved_stdout, STDOUT_FILENO) == -1)
    {
        perror("dup2 failed while restoring stdout");
        goto cleanup;
    }
    close(saved_stdout);
    saved_stdout = -1;

    if (lseek(out_fd, 0, SEEK_SET) == -1)
    {
        perror("lseek failed for directory output capture");
        goto cleanup;
    }

    ssize_t n = read(out_fd, output, output_size - 1);
    if (n < 0)
    {
        perror("read failed for directory output capture");
        goto cleanup;
    }
    output[n] = '\0';
    ok = true;

cleanup:
    if (saved_stdout != -1)
    {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (out_fd != -1)
        close(out_fd);
    unlink(out_template);
    return ok;
}

/**
 * Nested directory search test
 */
void test_recursive_directory_search(void)
{
    printf("\n=== Testing Recursive Directory Search ===\n");

    // Setup: Create test directory structure
    create_test_directory_structure();

    // Configure basic search parameters
    search_params_t params = {0};
    params.patterns = malloc(sizeof(char *));
    params.pattern_lens = malloc(sizeof(size_t));
    if (!params.patterns || !params.pattern_lens)
    {
        fprintf(stderr, "Failed to allocate memory for test parameters\n");
        cleanup_test_directory_structure();
        return;
    }

    // Pattern that exists only in some files
    const char *test_pattern = "FINDME";
    params.patterns[0] = (char *)test_pattern;
    params.pattern_lens[0] = strlen(test_pattern);
    params.num_patterns = 1;
    params.case_sensitive = true;
    params.use_regex = false;
    params.count_lines_mode = true;
    params.track_positions = false;
    params.max_count = SIZE_MAX;

    // Execute recursive search on the test directory and capture stdout.
    printf("Testing recursive search for pattern '%s'...\n", test_pattern);
    char output_buffer[32768];
    int errors = -1;
    bool capture_ok = run_recursive_search_capture(TEST_DIR_BASE, &params, 1,
                                                   output_buffer, sizeof(output_buffer), &errors);
    TEST_ASSERT(capture_ok, "Capture recursive directory search output");

    if (capture_ok)
    {
        TEST_ASSERT(errors == 0, "Recursive directory search completes without errors");
        TEST_ASSERT(strstr(output_buffer, "file1.txt") != NULL, "Recursive search finds match in file1.txt");
        TEST_ASSERT(strstr(output_buffer, "file3.log") == NULL, "Skips .log files from recursive search");
        TEST_ASSERT(strstr(output_buffer, "file_level0_1.txt") != NULL ||
                        strstr(output_buffer, "file_level0_2.txt") != NULL,
                    "Recursive search finds match in nested directories");

        // Ensure known skipped paths/extensions are not reported as matches.
        TEST_ASSERT(strstr(output_buffer, ".git/file_in_git.txt") == NULL, "Skips .git directory files");
        TEST_ASSERT(strstr(output_buffer, "node_modules/file_in_modules.txt") == NULL, "Skips node_modules files");
        TEST_ASSERT(strstr(output_buffer, "minified.min.js") == NULL, "Skips .min.* files");
        TEST_ASSERT(strstr(output_buffer, "image.jpg") == NULL, "Skips configured binary-like extensions");
    }

    // Cleanup
    free(params.patterns);
    free(params.pattern_lens);
    cleanup_test_directory_structure();
}

/**
 * Binary file handling test
 */
void test_binary_file_handling(void)
{
    printf("\n=== Testing Binary File Handling ===\n");

    // Create a test binary file
    const char *binary_file_path = "/tmp/krep_test_binary.bin";
    create_binary_file(binary_file_path);

    // Configure search parameters
    search_params_t params = {0};
    params.patterns = malloc(sizeof(char *));
    params.pattern_lens = malloc(sizeof(size_t));
    if (!params.patterns || !params.pattern_lens)
    {
        fprintf(stderr, "Failed to allocate memory for test parameters\n");
        unlink(binary_file_path);
        return;
    }

    // Pattern that might exist in the binary file (we'll search for 'AB' which will be present)
    const char *test_pattern = "AB";
    params.patterns[0] = (char *)test_pattern;
    params.pattern_lens[0] = strlen(test_pattern);
    params.num_patterns = 1;
    params.case_sensitive = true;
    params.use_regex = false;
    params.count_lines_mode = true;
    params.track_positions = false;
    params.max_count = SIZE_MAX;

    // Execute search on the binary file
    printf("Testing search on binary file...\n");
    int result = search_file(&params, binary_file_path, 1);

    // Verify results (should complete safely and not report internal errors).
    TEST_ASSERT(result != 2, "Binary file search completes without internal errors");

    // Cleanup
    free(params.patterns);
    free(params.pattern_lens);
    unlink(binary_file_path);
}

/**
 * Main function for test execution
 */
int main(void)
{
    printf("Starting Directory and Binary File Tests\n");

    // Verify permissions to create test directories and files
    if (access("/tmp", W_OK) != 0)
    {
        fprintf(stderr, "Cannot write to /tmp, skipping tests\n");
        return 1;
    }

    // Run tests
    test_recursive_directory_search();
    test_binary_file_handling();

    printf("\n=== Directory Test Summary ===\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("Total tests run: %d\n", tests_passed + tests_failed);
    return (tests_failed == 0) ? 0 : 1;
}

/* ========================================================================= */
/* Test support functions                                                    */
/* ========================================================================= */

/**
 * Creates a test directory structure
 */
static void create_test_directory_structure(void)
{
    printf("Creating test directory structure at %s\n", TEST_DIR_BASE);

    // Remove any existing directories
    cleanup_test_directory_structure();

    // Create base directory
    mkdir(TEST_DIR_BASE, 0755);

    // Create regular test files
    create_text_file(TEST_DIR_BASE "/file1.txt", "This is a text file\nIt has the pattern FINDME here\nAnd more text");
    create_text_file(TEST_DIR_BASE "/file2.txt", "This file doesn't have the pattern\nJust normal text");
    create_text_file(TEST_DIR_BASE "/file3.log", "Log file with FINDME pattern\nMultiple times FINDME");

    // Create symbolic link to test that it's handled correctly
    symlink(TEST_DIR_BASE "/file1.txt", TEST_DIR_BASE "/link_to_file1.txt");

    // Create directories to skip (should_skip_directory)
    mkdir(TEST_DIR_BASE "/.git", 0755);
    create_text_file(TEST_DIR_BASE "/.git/file_in_git.txt", "This has FINDME but should be skipped");

    mkdir(TEST_DIR_BASE "/node_modules", 0755);
    create_text_file(TEST_DIR_BASE "/node_modules/file_in_modules.txt", "This has FINDME but should be skipped");

    // Create nested directories
    create_nested_directory(TEST_DIR_BASE, 0, 3);

    // Create files with extensions to skip
    create_binary_file(TEST_DIR_BASE "/binary.exe");
    create_text_file(TEST_DIR_BASE "/minified.min.js", "function minified(){console.log('FINDME')}");
    create_text_file(TEST_DIR_BASE "/image.jpg", "Not a real image but should be skipped FINDME");

    printf("Test directory structure created\n");
}

/**
 * Cleans up the test directory structure
 */
static void cleanup_test_directory_structure(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR_BASE);
    system(cmd);
}

/**
 * Creates a test binary file
 */
static void create_binary_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "Failed to create binary file: %s\n", path);
        return;
    }

    // Create binary data with NULL bytes and recognizable patterns
    unsigned char data[TEST_FILE_MAX_SIZE];
    for (int i = 0; i < TEST_FILE_MAX_SIZE; i++)
    {
        if (i % 32 == 0)
        {
            data[i] = 0; // NULL byte every 32 bytes
        }
        else if (i % 128 < 2)
        {
            data[i] = 'A' + (i % 2); // Insert "AB" every 128 bytes
        }
        else
        {
            data[i] = (i % 95) + 32; // Printable ASCII characters
        }
    }

    fwrite(data, 1, TEST_FILE_MAX_SIZE, f);
    fclose(f);
}

/**
 * Creates a text file with specific content
 */
static void create_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "Failed to create text file: %s\n", path);
        return;
    }

    fputs(content, f);
    fclose(f);
}

/**
 * Creates nested directories with test files
 */
static void create_nested_directory(const char *base_path, int depth, int max_depth)
{
    if (depth >= max_depth)
        return;

    // Create 2 subdirectories for each level
    for (int i = 1; i <= 2; i++)
    {
        char subdir_path[PATH_MAX];
        snprintf(subdir_path, sizeof(subdir_path), "%s/level%d_%d", base_path, depth, i);

        mkdir(subdir_path, 0755);

        // Create files in this directory
        char file_path[PATH_MAX];
        snprintf(file_path, sizeof(file_path), "%s/file_level%d_%d.txt", subdir_path, depth, i);

        // Put the pattern only in files at even levels
        if (depth % 2 == 0)
        {
            create_text_file(file_path, "This file has FINDME pattern in nested directory");
        }
        else
        {
            create_text_file(file_path, "This file doesn't have the pattern");
        }

        // Recursion
        create_nested_directory(subdir_path, depth + 1, max_depth);
    }
}
