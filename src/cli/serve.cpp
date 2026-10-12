// SPDX-License-Identifier: Apache-2.0
/**
 * @file serve.cpp
 * @brief `entropic serve` — a headless host for every MCP client (gh#204).
 *
 * Configures one engine from a YAML file, serves its external bridge at
 * `http://127.0.0.1:<port>/mcp`, and runs until SIGINT or SIGTERM. Every MCP
 * client points at that one URL with the bearer token from
 * `~/.entropic/serve/<port>.token`, instead of each spawning its own host.
 *
 * Exits non-zero, with the engine's own reason, if the engine cannot be
 * configured or the port cannot be served — a host that is not reachable
 * must not look like one that is.
 *
 * @version 2.14.0
 */

#include <entropic/entropic.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <pthread.h>

namespace entropic::cli {

namespace {

/**
 * @brief Parsed `serve` arguments.
 * @version 2.14.0
 */
struct ServeArgs {
    std::string config;  ///< --config FILE
    std::string port;    ///< --port N
};

/**
 * @brief Read --config and --port; anything else is a usage error.
 * @param argc Argument count (argv[0] is "serve").
 * @param argv Arguments.
 * @param[out] out Parsed arguments.
 * @return true when both were given and nothing unknown was.
 * @utility
 * @version 2.14.0
 */
bool parse_args(int argc, char* argv[], ServeArgs& out) {
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--config" && has_value) {
            out.config = argv[++i];
        } else if (flag == "--port" && has_value) {
            out.port = argv[++i];
        } else {
            ok = false;
        }
    }
    return ok && !out.config.empty() && !out.port.empty();
}

/**
 * @brief Block the signals we wait for, before any engine thread exists,
 *        so every thread inherits the mask and sigwait() gets them.
 * @param set Filled with SIGINT and SIGTERM.
 * @utility
 * @version 2.14.0
 */
void block_shutdown_signals(sigset_t& set) {
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

/**
 * @brief Configure the engine, serving HTTP on the requested port.
 * @param args Parsed arguments.
 * @param[out] handle Created handle (destroy it even on failure).
 * @return ENTROPIC_OK, or the configure error.
 * @utility
 * @version 2.14.0
 */
entropic_error_t configure_host(const ServeArgs& args,
                                entropic_handle_t* handle) {
    // The env overrides apply on every configure path, so the port set here
    // wins over whatever the file says without a second config format.
    setenv("ENTROPIC_MCP__EXTERNAL__ENABLED", "true", 1);
    setenv("ENTROPIC_MCP__EXTERNAL__HTTP_PORT", args.port.c_str(), 1);
    entropic_error_t rc = entropic_create(handle);
    if (rc == ENTROPIC_OK) {
        rc = entropic_configure_from_file(*handle, args.config.c_str());
    }
    return rc;
}

/**
 * @brief Print usage for `serve`.
 * @utility
 * @version 2.14.0
 */
void print_usage() {
    std::fprintf(stderr,
        "usage: entropic serve --config FILE --port N\n"
        "\n"
        "Serve the engine to every MCP client at http://127.0.0.1:N/mcp.\n"
        "Clients send 'Authorization: Bearer <token>', the token being the\n"
        "contents of ~/.entropic/serve/N.token (regenerated each start).\n"
        "Loopback only; for remote use, tunnel: ssh -L N:127.0.0.1:N host.\n"
        "Runs until SIGINT or SIGTERM.\n");
}

}  // namespace

/**
 * @brief `entropic serve` entry point.
 * @param argc Argument count (argv[0] is "serve").
 * @param argv Arguments.
 * @return 0 after a clean shutdown; 1 on usage or startup failure.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
int run_serve(int argc, char* argv[]) {
    ServeArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 1;
    }
    sigset_t set;
    block_shutdown_signals(set);
    entropic_handle_t handle = nullptr;
    const entropic_error_t rc = configure_host(args, &handle);
    if (rc != ENTROPIC_OK) {
        const char* why = handle != nullptr ? entropic_last_error(handle) : nullptr;
        std::fprintf(stderr, "entropic serve: cannot start (%d): %s\n",
                     static_cast<int>(rc), why != nullptr ? why : "unknown");
    } else {
        std::fprintf(stderr,
            "entropic serve: http://127.0.0.1:%s/mcp (token: %s/.entropic/serve/%s.token)\n",
            args.port.c_str(),
            std::getenv("HOME") != nullptr ? std::getenv("HOME") : "~",
            args.port.c_str());
        int sig = 0;
        sigwait(&set, &sig);
        std::fprintf(stderr, "entropic serve: %s, shutting down\n",
                     sig == SIGTERM ? "SIGTERM" : "SIGINT");
    }
    entropic_destroy(handle);  // stops the listener, removes the token
    return rc == ENTROPIC_OK ? 0 : 1;
}

}  // namespace entropic::cli
