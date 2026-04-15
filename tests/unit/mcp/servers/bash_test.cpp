// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_bash.cpp
 * @brief BashServer unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/bash.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
using namespace entropic;

// ── Helpers ─────────────────────────────────────────────

/**
 * @brief Create a BashServer with TEST_DATA_DIR and /tmp as working dir.
 * @internal
 * @version 1.8.5
 */
static BashServer make_bash_server(const std::filesystem::path& working_dir = "/tmp") {
    return BashServer(working_dir, TEST_DATA_DIR, 10);
}

/**
 * @brief Parse the "result" field from execute() JSON envelope.
 * @internal
 * @param envelope Raw JSON string returned by execute().
 * @return The "result" string value.
 * @version 1.8.5
 */
static std::string parse_result(const std::string& envelope) {
    auto j = json::parse(envelope);
    return j["result"].get<std::string>();
}

/**
 * @brief RAII temp directory that cleans up on destruction.
 * @internal
 * @version 1.8.5
 */
struct TempDir {
    std::filesystem::path path;

    /**
     * @brief Create a unique temp directory.
     * @internal
     * @version 1.8.5
     */
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("entropic_bash_test_" + std::to_string(
                   std::hash<std::string>{}(
                       std::to_string(reinterpret_cast<uintptr_t>(this)))));
        std::filesystem::create_directories(path);
    }

    /**
     * @brief Remove the temp directory.
     * @internal
     * @version 1.8.5
     */
    ~TempDir() {
        std::filesystem::remove_all(path);
    }
};

// ── Tests ───────────────────────────────────────────────

TEST_CASE("Execute captures stdout output", "[bash]") {
    auto server = make_bash_server();
    json args;
    args["command"] = "echo hello";

    auto result = parse_result(server.execute("execute", args.dump()));
    REQUIRE(result.find("hello") != std::string::npos);
}

TEST_CASE("Execute captures stderr output", "[bash]") {
    auto server = make_bash_server();
    json args;
    args["command"] = "ls /nonexistent_path_xyz";

    auto result = parse_result(server.execute("execute", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Execute respects working directory", "[bash]") {
    TempDir tmp;
    {
        std::ofstream f(tmp.path / "sentinel_file.txt");
        f << "present";
    }

    auto server = make_bash_server(tmp.path);
    json args;
    args["command"] = "ls";

    auto result = parse_result(server.execute("execute", args.dump()));
    REQUIRE(result.find("sentinel_file.txt") != std::string::npos);
}

TEST_CASE("Execute nonexistent command returns error", "[bash]") {
    auto server = make_bash_server();
    json args;
    args["command"] = "this_command_does_not_exist_xyz";

    auto result = parse_result(server.execute("execute", args.dump()));
    REQUIRE_FALSE(result.empty());
}
