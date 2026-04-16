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
int run_download(int argc, char* argv[]);

} // namespace entropic::cli

/**
 * @brief Print top-level usage to stderr.
 * @utility
 * @version 2.0.5
 */
static void print_usage()
{
    std::fprintf(stderr,
        "Usage: entropic <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  mcp-bridge   Run as MCP server over stdio (JSON-RPC 2.0)\n"
        "  download     Fetch a bundled GGUF model\n"
        "  version      Print engine version\n"
        "\n"
        "Options for mcp-bridge:\n"
        "  --project-dir DIR   Project config directory (default: cwd)\n"
        "\n"
        "Options for download:\n"
        "  --list              List available model keys\n"
        "  --dir DIR           Override target directory\n"
        "                      (default: $ENTROPIC_MODEL_DIR or ~/.entropic/models)\n"
        "\n"
        "Example .mcp.json entry:\n"
        "  {\"mcpServers\": {\"entropic\": {\n"
        "    \"type\": \"stdio\", \"command\": \"entropic\",\n"
        "    \"args\": [\"mcp-bridge\"]\n"
        "  }}}\n");
}

namespace {

using SubcommandFn = int (*)(int, char*[]);

struct Subcommand {
    const char* name;
    SubcommandFn run;
};

/**
 * @brief Adapter for `version` which takes no args.
 * @internal
 * @return Subcommand exit code.
 * @version 2.0.5
 */
int run_version_adapter(int, char*[])
{
    return entropic::cli::run_version();
}

constexpr Subcommand kSubcommands[] = {
    {"mcp-bridge", entropic::cli::run_mcp_bridge},
    {"version",    run_version_adapter},
    {"download",   entropic::cli::run_download},
};

} // anonymous namespace

/**
 * @brief CLI entry point — dispatch to subcommand.
 *
 * @param argc Argument count.
 * @param argv Argument vector. argv[1] is the subcommand name.
 * @return 0 on success, 1 on usage error or subcommand failure.
 *
 * @internal
 * @version 2.0.5
 */
int main(int argc, char* argv[])
{
    std::string sub = (argc >= 2) ? argv[1] : "";
    for (const auto& entry : kSubcommands) {
        if (sub == entry.name) {
            return entry.run(argc - 1, argv + 1);
        }
    }
    bool help = (sub == "--help" || sub == "-h" || sub == "help");
    if (!help && !sub.empty()) {
        std::fprintf(stderr, "entropic: unknown subcommand '%s'\n\n",
                     sub.c_str());
    }
    print_usage();
    return help ? 0 : 1;
}
