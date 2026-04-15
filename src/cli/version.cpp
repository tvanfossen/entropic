/**
 * @file version.cpp
 * @brief `entropic version` subcommand.
 * @version 2.0.3
 */

#include <entropic/entropic_config.h>

#include <cstdio>

namespace entropic::cli {

/**
 * @brief Print the engine version and exit.
 * @return 0.
 * @internal
 * @version 2.0.3
 */
int run_version()
{
    std::printf("entropic %s\n", CONFIG_ENTROPIC_VERSION_STRING);
    return 0;
}

} // namespace entropic::cli
