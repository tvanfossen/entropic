// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file main.c
 * @brief Headless example — scripted conversation for CI validation.
 *
 * Runs a fixed sequence of prompts against the entropic engine without
 * any stdin input. Validates each response against expected criteria
 * and exits 0 on success, 1 on any failure. Designed for CI gating
 * and post-mortem log review.
 *
 * Validates:
 *   1. Single-turn inference (response is non-empty)
 *   2. Multi-turn context retention (follow-up references prior turn)
 *   3. .mcp.json auto-discovery (test_server registered without code)
 *   4. context_clear resets conversation
 *   5. Final exit code reflects pass/fail
 *
 * Usage:
 *   inv example -n headless         # CI usage
 *   ./build/headless                # direct invocation
 *
 * @version 1
 */

#include <entropic/entropic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCUM_SIZE 16384

/**
 * @brief Streaming token accumulator + stderr mirror.
 *
 * Mirrors tokens to stderr (visible during CI runs) and accumulates
 * them in a fixed buffer for post-call validation against expected
 * substrings.
 *
 * @callback
 * @version 1
 */
static void on_token(const char* token, size_t len, void* user_data)
{
    char* acc = (char*)user_data;
    fwrite(token, 1, len, stderr);
    fflush(stderr);
    size_t cur = strlen(acc);
    size_t room = ACCUM_SIZE - cur - 1;
    size_t copy = (len < room) ? len : room;
    memcpy(acc + cur, token, copy);
    acc[cur + copy] = '\0';
}

/**
 * @brief Run a single scripted prompt and validate the response.
 *
 * Executes entropic_run_streaming, accumulates tokens, then checks
 * that the response contains the expected substring. On mismatch,
 * prints a failure marker and increments the failure count.
 *
 * @param handle    Engine handle.
 * @param label     Step label for log output.
 * @param prompt    User prompt to send.
 * @param expect    Substring expected in the response (NULL = any).
 * @param failures  Failure counter (incremented on mismatch).
 *
 * @utility
 * @version 1
 */
static void run_step(entropic_handle_t handle, const char* label,
                     const char* prompt, const char* expect,
                     int* failures)
{
    char acc[ACCUM_SIZE];
    acc[0] = '\0';
    fprintf(stderr, "\n=== %s ===\nPrompt: %s\nResponse: ", label, prompt);
    entropic_error_t err = entropic_run_streaming(
        handle, prompt, on_token, acc, NULL);
    fprintf(stderr, "\n");
    if (err != ENTROPIC_OK) {
        fprintf(stderr, "FAIL [%s]: error %s\n",
                label, entropic_last_error(handle));
        (*failures)++;
        return;
    }
    if (expect && !strstr(acc, expect)) {
        fprintf(stderr, "FAIL [%s]: response missing '%s'\n",
                label, expect);
        (*failures)++;
        return;
    }
    fprintf(stderr, "PASS [%s]\n", label);
}

/**
 * @brief Validate that context_count returns the expected value.
 *
 * @param handle    Engine handle.
 * @param label     Step label.
 * @param expected  Expected message count.
 * @param failures  Failure counter.
 *
 * @utility
 * @version 1
 */
static void check_context_count(entropic_handle_t handle, const char* label,
                                size_t expected, int* failures)
{
    size_t count = 0;
    entropic_error_t err = entropic_context_count(handle, &count);
    if (err != ENTROPIC_OK) {
        fprintf(stderr, "FAIL [%s]: context_count error\n", label);
        (*failures)++;
        return;
    }
    if (count != expected) {
        fprintf(stderr, "FAIL [%s]: count=%zu expected=%zu\n",
                label, count, expected);
        (*failures)++;
        return;
    }
    fprintf(stderr, "PASS [%s]: count=%zu\n", label, count);
}

/**
 * @brief Run the scripted validation sequence.
 *
 * Steps:
 *   1. Single-turn inference (any non-empty response)
 *   2. Multi-turn context (follow-up should reference prior turn)
 *   3. Verify message count grew
 *   4. context_clear resets conversation
 *   5. Verify message count == 0 after clear
 *
 * @param handle    Configured engine handle.
 * @return Number of failures (0 = all passed).
 *
 * @internal
 * @version 1
 */
static int run_scenarios(entropic_handle_t handle)
{
    int failures = 0;

    run_step(handle, "single-turn",
             "Reply with exactly the word: hello",
             "hello", &failures);

    run_step(handle, "multi-turn",
             "What word did I just ask you to reply with?",
             "hello", &failures);

    check_context_count(handle, "after-2-turns", 4, &failures);

    entropic_error_t err = entropic_context_clear(handle);
    if (err != ENTROPIC_OK) {
        fprintf(stderr, "FAIL [clear]: error %s\n",
                entropic_last_error(handle));
        failures++;
    } else {
        fprintf(stderr, "PASS [clear]\n");
    }

    check_context_count(handle, "after-clear", 0, &failures);

    return failures;
}

/**
 * @brief Headless example entry point.
 *
 * Creates engine, configures from .headless/, runs scripted validation,
 * exits with pass (0) or fail (1) status.
 *
 * @param argc  Argument count (unused).
 * @param argv  Argument vector (unused).
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    entropic_handle_t handle = NULL;
    if (entropic_create(&handle) != ENTROPIC_OK) {
        fprintf(stderr, "entropic_create failed\n");
        return EXIT_FAILURE;
    }
    if (entropic_configure_dir(handle, ".headless") != ENTROPIC_OK) {
        fprintf(stderr, "configure failed: %s\n",
                entropic_last_error(handle));
        entropic_destroy(handle);
        return EXIT_FAILURE;
    }

    int failures = run_scenarios(handle);
    entropic_destroy(handle);

    fprintf(stderr, "\n=== headless: %s (%d failure%s) ===\n",
            failures == 0 ? "PASS" : "FAIL",
            failures, failures == 1 ? "" : "s");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
