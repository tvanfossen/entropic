// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_bash.cpp
 * @brief BashServer unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/bash.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <thread>

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

// ── v2.3.10: coverage for permission-pattern + accessors ──

TEST_CASE("get_permission_pattern extracts the base command",
          "[bash][v2.3.10][coverage]") {
    auto server = make_bash_server();

    SECTION("multi-word command — base is the first token") {
        json args;
        args["command"] = "ls -la /tmp";
        auto pat = server.get_permission_pattern(
            "bash.execute", args.dump());
        REQUIRE(pat.find("ls") != std::string::npos);
        REQUIRE(pat.find("*") != std::string::npos);
    }

    SECTION("single-word command — base equals the whole command") {
        json args;
        args["command"] = "pwd";
        auto pat = server.get_permission_pattern(
            "bash.execute", args.dump());
        REQUIRE(pat.find("pwd") != std::string::npos);
    }

    SECTION("malformed JSON args — falls back to 'unknown' base") {
        // The catch path replaces base with "unknown".
        auto pat = server.get_permission_pattern(
            "bash.execute", "not-json");
        REQUIRE(pat.find("unknown") != std::string::npos);
    }
}

TEST_CASE("load_tool_definition throws when the JSON file is missing",
          "[tool_base][v2.3.10][coverage][failure-mode]") {
    REQUIRE_THROWS_AS(
        entropic::load_tool_definition(
            "does_not_exist_v2310", "bash", "/tmp/nonexistent-data-dir"),
        std::runtime_error);
}

TEST_CASE("BashServer working_dir setter/getter + timeout accessor",
          "[bash][v2.3.10][coverage]") {
    auto server = make_bash_server();

    auto initial = server.working_dir();
    REQUIRE(server.set_working_dir(std::filesystem::temp_directory_path()
                                     .string()));
    auto updated = server.working_dir();
    REQUIRE(updated == std::filesystem::temp_directory_path());

    // timeout() simply returns the constructed timeout; just call it
    // for coverage and assert non-negative.
    REQUIRE(server.timeout() >= 0);

    // Reset back so any subsequent test in this binary isn't surprised
    // by a different cwd.
    server.set_working_dir(initial.string());
}

// ── v2.13.0: the timeout is ENFORCED, on the whole process group ──
//
// Until v2.13.0 `BashServer::timeout_` was stored, logged and never read,
// and popen() gave no way to stop a command: `sleep 30` held the tool call
// (and the run thread, and every lock above it) for 30 s, and a command's
// background children outlived it. Bounds below are generous on purpose —
// the enforced limit is 1 s, the assertion allows 10 s — so a loaded
// machine cannot flake them; the unenforced behaviour takes 20-25 s.

namespace {

/**
 * @brief Whether any LIVE process (not a zombie) is in process group `pgid`.
 *
 * Scans /proc/<pid>/stat (field 3 state, field 5 pgrp). A zombie keeps its
 * group until its parent reaps it, and a killed grandchild is reaped by
 * init on init's schedule, so a zombie counts as dead here.
 *
 * @param pgid Process group to look for.
 * @return true when some non-zombie process has that pgrp.
 * @internal
 * @version 2.13.0
 */
bool live_in_group(pid_t pgid) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const auto name = e.path().filename().string();
        if (name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::ifstream in(e.path() / "stat");
        std::string stat;
        std::getline(in, stat);
        auto close = stat.rfind(')');
        if (close == std::string::npos || close + 2 >= stat.size()) {
            continue;
        }
        std::istringstream rest(stat.substr(close + 2));
        char state = 0;
        long ppid = 0;
        long pgrp = 0;
        rest >> state >> ppid >> pgrp;
        if (pgrp == pgid && state != 'Z' && state != 'X') { return true; }
    }
    return false;
}

/**
 * @brief Whether `pid` is a live (running, non-zombie) process.
 * @param pid Process id.
 * @return false when it is gone or a zombie.
 * @internal
 * @version 2.13.0
 */
bool alive(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(in, stat)) { return false; }
    auto close = stat.rfind(')');
    return close != std::string::npos && close + 2 < stat.size()
        && stat[close + 2] != 'Z' && stat[close + 2] != 'X';
}

/**
 * @brief Poll `pred` until it holds or `limit` passes.
 * @param pred Condition.
 * @param limit Upper bound.
 * @return Whether it held.
 * @internal
 * @version 2.13.0
 */
template <typename Pred>
bool eventually(Pred pred, std::chrono::seconds limit) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

/**
 * @brief The integer printed after `tag=` in `text`, or -1.
 * @param text Command output.
 * @param tag Marker such as "pid".
 * @return The number, or -1 when absent.
 * @internal
 * @version 2.13.0
 */
pid_t marker(const std::string& text, const std::string& tag) {
    auto at = text.find(tag + "=");
    if (at == std::string::npos) { return -1; }
    return static_cast<pid_t>(std::stol(text.substr(at + tag.size() + 1)));
}

/**
 * @brief Run `command` through a server with `timeout_s`, timing it.
 * @param timeout_s Server timeout in seconds.
 * @param command Shell command.
 * @param[out] elapsed Wall time of the execute() call.
 * @return The tool's result text.
 * @internal
 * @version 2.13.0
 */
std::string timed_run(int timeout_s, const std::string& command,
                      std::chrono::milliseconds& elapsed) {
    BashServer server("/tmp", TEST_DATA_DIR, timeout_s);
    json args;
    args["command"] = command;
    auto t0 = std::chrono::steady_clock::now();
    auto result = parse_result(server.execute("execute", args.dump()));
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    return result;
}

}  // namespace

TEST_CASE("bash: a command that outlives the timeout is killed with its "
          "whole process group, and the model is told why",
          "[bash][timeout][v2.13.0]") {
    std::chrono::milliseconds elapsed{0};
    // $$ is the shell's pid, which is also the group id if the command
    // runs in its own group.
    auto text = timed_run(1, "echo pid=$$; sleep 20", elapsed);
    INFO("result: " << text << " elapsed_ms=" << elapsed.count());

    CHECK(elapsed < std::chrono::seconds(10));  // unenforced: ~20 s
    auto j = json::parse(text, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j.value("error", "") == "timeout");
    CHECK(j.value("timeout_seconds", -1) == 1);
    CHECK(j.value("elapsed_ms", -1) >= 1000);
    const auto message = j.value("message", "");
    CHECK(message.find("1 s") != std::string::npos);
    CHECK(message.find("killed") != std::string::npos);
    // What the command printed before it was killed is not thrown away.
    const auto shell = marker(j.value("output", ""), "pid");
    REQUIRE(shell > 0);
    // The shell is REAPED, not a zombie: kill(pid, 0) finds nothing.
    errno = 0;
    CHECK(::kill(shell, 0) == -1);
    CHECK(errno == ESRCH);
    // And nothing in its process group survives (sleep included).
    CHECK(eventually([&] { return !live_in_group(shell); },
                     std::chrono::seconds(5)));
}

TEST_CASE("bash: a background child is not left running after the "
          "command returns", "[bash][timeout][v2.13.0]") {
    std::chrono::milliseconds elapsed{0};
    // Output detached, so the child does not hold the pipe: the call
    // returns at once today and the child keeps running for 30 s.
    auto text = timed_run(10, "sleep 30 >/dev/null 2>&1 & echo bg=$!",
                          elapsed);
    INFO("result: " << text << " elapsed_ms=" << elapsed.count());
    auto j = json::parse(text, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j.value("exit_code", -1) == 0);
    const auto bg = marker(j.value("output", ""), "bg");
    REQUIRE(bg > 0);
    CHECK(eventually([&] { return !alive(bg); }, std::chrono::seconds(5)));
    if (alive(bg)) { ::kill(bg, SIGKILL); }  // never leak from the test
}

TEST_CASE("bash: a background child holding the output pipe does not hang "
          "the call", "[bash][timeout][v2.13.0]") {
    std::chrono::milliseconds elapsed{0};
    // The child inherits stdout, so the pipe never reaches EOF while it
    // lives: popen's read loop blocked for the child's whole lifetime.
    auto text = timed_run(20, "sleep 25 & echo bg=$!", elapsed);
    INFO("result: " << text << " elapsed_ms=" << elapsed.count());
    CHECK(elapsed < std::chrono::seconds(10));  // unenforced: ~25 s
    auto j = json::parse(text, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j.value("exit_code", -1) == 0);  // the COMMAND finished cleanly
    const auto bg = marker(j.value("output", ""), "bg");
    REQUIRE(bg > 0);
    CHECK(eventually([&] { return !alive(bg); }, std::chrono::seconds(5)));
    if (alive(bg)) { ::kill(bg, SIGKILL); }
}

TEST_CASE("bash: a command inside its limit still reports its own exit code "
          "and output", "[bash][timeout][v2.13.0]") {
    std::chrono::milliseconds elapsed{0};
    auto text = timed_run(10, "echo out; echo err >&2; exit 3", elapsed);
    INFO("result: " << text);
    auto j = json::parse(text, nullptr, false);
    REQUIRE(j.is_object());
    CHECK_FALSE(j.contains("error"));
    CHECK(j.value("exit_code", -1) == 3);
    const auto out = j.value("output", "");
    CHECK(out.find("out") != std::string::npos);
    CHECK(out.find("err") != std::string::npos);
}
