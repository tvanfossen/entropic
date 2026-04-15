// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file main.cpp
 * @brief entropic CLI binary — top-level dispatcher for subcommands.
 *
 * Provides a single `entropic` executable that ships with the engine.
 * Subcommands let users integrate the engine into other systems with
 * zero code:
 *
 *   entropic mcp-bridge [--project-dir DIR]
 *     Speaks JSON-RPC 2.0 over stdio so external MCP clients (Claude
 *     Code, VSCode, etc.) can use the engine as a tool provider.
 *
 *   entropic version
 *     Print engine version and exit.
 *
 * The mcp-bridge subcommand is the primary integration point: a user
 * adds entropic to their .mcp.json with `command: "entropic"` and
 * `args: ["mcp-bridge"]`, and the engine becomes available as MCP
 * tools without writing any code.
 *
 * @version 2.0.3
 */

#include <cstdio>
#include <cstring>
#include <string>

namespace entropic::cli {

int run_mcp_bridge(int argc, char* argv[]);
int run_version();

} // namespace entropic::cli

/**
 * @brief Print top-level usage to stderr.
 * @utility
 * @version 2.0.3
 */
static void print_usage()
{
    std::fprintf(stderr,
        "Usage: entropic <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  mcp-bridge   Run as MCP server over stdio (JSON-RPC 2.0)\n"
        "  version      Print engine version\n"
        "\n"
        "Options for mcp-bridge:\n"
        "  --project-dir DIR   Project config directory (default: cwd)\n"
        "\n"
        "Example .mcp.json entry:\n"
        "  {\"mcpServers\": {\"entropic\": {\n"
        "    \"type\": \"stdio\", \"command\": \"entropic\",\n"
        "    \"args\": [\"mcp-bridge\"]\n"
        "  }}}\n");
}

/**
 * @brief CLI entry point — dispatch to subcommand.
 *
 * @param argc Argument count.
 * @param argv Argument vector. argv[1] is the subcommand name.
 * @return 0 on success, 1 on usage error or subcommand failure.
 *
 * @internal
 * @version 2.0.3
 */
int main(int argc, char* argv[])
{
    std::string sub = (argc >= 2) ? argv[1] : "";
    if (sub == "mcp-bridge") {
        return entropic::cli::run_mcp_bridge(argc - 1, argv + 1);
    }
    if (sub == "version") {
        return entropic::cli::run_version();
    }
    bool help = (sub == "--help" || sub == "-h" || sub == "help");
    if (!help && !sub.empty()) {
        std::fprintf(stderr, "entropic: unknown subcommand '%s'\n\n",
                     sub.c_str());
    }
    print_usage();
    return help ? 0 : 1;
}
