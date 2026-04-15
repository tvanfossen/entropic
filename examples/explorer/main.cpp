// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file main.cpp
 * @brief Entropic Explorer — interactive architecture guide.
 *
 * Demonstrates multi-tier delegation (guide + analyst + quiz_master),
 * grammar constraints, external MCP server registration, and streaming
 * output. The docs MCP server (docs_server.py) runs as an external
 * process over stdio transport, querying the doxygen SQLite database.
 *
 * Usage:
 *   1. Generate docs DB: inv docs --enrich
 *   2. Build: cmake -B build && cmake --build build
 *   3. Run: ./build/explorer
 *
 * @version 1
 */

#include <entropic/entropic.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* ── Streaming ───────────────────────────────────────── */

/**
 * @brief Streaming token callback — prints to stderr for live output.
 *
 * @param token      Token bytes (not null-terminated).
 * @param len        Token byte length.
 * @param user_data  Unused.
 *
 * @callback
 * @version 1
 */
static void on_token(const char* token, size_t len, void* user_data)
{
    (void)user_data;
    std::fwrite(token, 1, len, stderr);
    std::fflush(stderr);
}

/* ── User input ──────────────────────────────────────── */

/**
 * @brief Read a line of user input from stdin.
 *
 * @param buf   Output buffer.
 * @param size  Buffer capacity.
 * @return true if input was read, false on quit/EOF.
 *
 * @utility
 * @version 1
 */
static bool read_input(char* buf, size_t size)
{
    std::printf("You: ");
    std::fflush(stdout);
    if (!std::fgets(buf, static_cast<int>(size), stdin)) {
        return false;
    }
    size_t len = std::strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    if (len == 0) {
        return true;
    }
    bool is_quit = (std::strcmp(buf, "quit") == 0
                 || std::strcmp(buf, "exit") == 0
                 || std::strcmp(buf, "q") == 0);
    return !is_quit;
}

/* ── Engine setup ────────────────────────────────────── */

/**
 * @brief Create, configure, and wire up the entropic engine.
 *
 * Creates a handle, configures via layered resolution from
 * .explorer/ directory, and registers docs_server.py as an
 * external MCP server over stdio transport.
 *
 * @param project_dir  Project config directory (e.g. ".explorer").
 * @return Configured engine handle, or nullptr on failure.
 *
 * @internal
 * @version 1
 */
static entropic_handle_t setup_engine(const char* project_dir)
{
    entropic_handle_t handle = nullptr;
    entropic_create(&handle);
    if (!handle) {
        std::fprintf(stderr, "entropic_create failed\n");
        return nullptr;
    }

    std::string docs_server = R"({"command":"python3","args":["servers/docs_server.py"]})";
    bool ok = (entropic_configure_dir(handle, project_dir) == ENTROPIC_OK)
           && (entropic_grammar_register_file(
                   handle, "quiz",
                   "data/grammars/quiz.gbnf") == ENTROPIC_OK)
           && (entropic_register_mcp_server(
                   handle, "docs", docs_server.c_str()) == ENTROPIC_OK);

    if (!ok) {
        std::fprintf(stderr, "Setup failed: %s\n", entropic_last_error(handle));
        entropic_destroy(handle);
        return nullptr;
    }
    return handle;
}

/* ── Interactive loop ────────────────────────────────── */

/**
 * @brief Run the interactive exploration loop.
 *
 * Reads user questions, runs streaming generation through the
 * guide tier (which may delegate to analyst or quiz_master).
 *
 * @param handle  Configured engine handle.
 *
 * @internal
 * @version 1
 */
static void explore_loop(entropic_handle_t handle)
{
    char input[4096];

    std::printf("\n");
    std::printf("Entropic Explorer — Repo Knowledge Assistant\n");
    std::printf("============================================\n");
    std::printf("Ask questions, review changes, learn architecture.\n");
    std::printf("  'review my changes'  — adversarial change analysis\n");
    std::printf("  'teach me about X'   — learn + quiz\n");
    std::printf("  'trace X'            — follow execution paths\n");
    std::printf("  'quit'               — exit\n\n");

    while (read_input(input, sizeof(input))) {
        if (input[0] == '\0') {
            continue;
        }
        std::fprintf(stderr, "\n");
        entropic_error_t err = entropic_run_streaming(
            handle, input, on_token, nullptr, nullptr);
        if (err != ENTROPIC_OK) {
            std::fprintf(stderr, "\nError: %s\n", entropic_last_error(handle));
        }
        std::printf("\n\n");
    }
}

/* ── Entry point ─────────────────────────────────────── */

/**
 * @brief Run the explorer application.
 *
 * Sets up the engine, runs the exploration loop, and cleans up.
 * Assumes CWD is the example directory.
 *
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
static int run_explorer()
{
    entropic_handle_t handle = setup_engine(".explorer");
    if (!handle) {
        return EXIT_FAILURE;
    }
    explore_loop(handle);
    entropic_destroy(handle);
    std::printf("Goodbye!\n");
    return EXIT_SUCCESS;
}

/**
 * @brief Entry point.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    return run_explorer();
}
