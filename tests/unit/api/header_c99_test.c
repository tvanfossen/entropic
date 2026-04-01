/**
 * @file test_header_c99.c
 * @brief Verifies entropic.h is valid C99.
 *
 * This file is compiled as pure C (not C++) to verify that entropic.h
 * and all headers it includes are C99-compatible with no C++ leakage.
 *
 * @version 1.8.9
 */

#include <entropic/entropic.h>

/**
 * @brief Verify entropic.h types and functions are visible in C99.
 * @return 0 on success.
 * @internal
 * @version 1.8.9
 */
int main(void) {
    /* Verify types are defined */
    entropic_handle_t h = NULL;
    entropic_error_t e = ENTROPIC_OK;
    entropic_hook_point_t hp = ENTROPIC_HOOK_PRE_GENERATE;
    entropic_hook_callback_t cb = NULL;
    const char* v = entropic_version();
    int api = entropic_api_version();
    (void)h; (void)e; (void)hp; (void)cb; (void)v; (void)api;
    return 0;
}
