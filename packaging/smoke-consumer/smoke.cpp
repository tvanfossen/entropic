// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file smoke.cpp
 * @brief Distribution smoke test — links entropic::entropic via
 *        find_package and calls zero-dependency entry points.
 * @version 2.0.5
 */

#include <entropic/entropic.h>

#include <cstdio>
#include <cstring>

/**
 * @brief Verify runtime version matches the packaged SOVERSION family.
 * @utility
 * @version 2.0.5
 */
int main()
{
    const char* ver = entropic_version();
    int api = entropic_api_version();

    if (ver == nullptr) {
        std::fputs("entropic_version() returned NULL\n", stderr);
        return 1;
    }

    std::printf("entropic runtime version: %s\n", ver);
    std::printf("entropic API version:     %d\n", api);

    // Minimum sanity: version starts with "2." and API >= 2.
    if (std::strncmp(ver, "2.", 2) != 0 || api < 2) {
        std::fputs("version/API sanity check failed\n", stderr);
        return 1;
    }

    return 0;
}
