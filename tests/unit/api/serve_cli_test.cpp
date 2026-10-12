// SPDX-License-Identifier: Apache-2.0
/**
 * @file serve_cli_test.cpp
 * @brief gh#204 (v2.14.0): `entropic serve` end to end, as a plugin uses it.
 *
 * Spawns the real binary with HOME pointed at a scratch directory, reads the
 * bearer token FROM THE TOKEN FILE, calls the engine over HTTP, then sends
 * SIGTERM and checks for a clean exit and a removed token. Startup failures
 * must exit non-zero with the engine's own reason — a host that is not
 * reachable must never look like one that is.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// @brief A scratch HOME plus a minimal config file inside it.
struct ServeScratch {
    fs::path home;
    fs::path config;
    fs::path stderr_log;
    ServeScratch() {
        home = fs::temp_directory_path()
            / ("entropic_serve_home_" + std::to_string(::getpid()));
        fs::remove_all(home);
        fs::create_directories(home);
        config = home / "serve.yaml";
        std::ofstream(config) << "log_level: INFO\n";
        stderr_log = home / "serve.stderr";
    }
    ~ServeScratch() {
        std::error_code ec;
        fs::remove_all(home, ec);
    }
    ServeScratch(const ServeScratch&) = delete;
    ServeScratch& operator=(const ServeScratch&) = delete;
};

/// @brief The file's full text ("" when absent).
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @brief A port nothing is listening on right now.
int free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/// @brief Fork `entropic serve <args>` with HOME=scratch, stderr to a file.
pid_t spawn_serve(const ServeScratch& s, const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid == 0) {
        setenv("HOME", s.home.c_str(), 1);
        int err = ::open(s.stderr_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(err, STDERR_FILENO);
        ::dup2(err, STDOUT_FILENO);
        ::chdir(s.home.c_str());
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(ENTROPIC_CLI_BIN_PATH));
        argv.push_back(const_cast<char*>("serve"));
        for (const auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
        argv.push_back(nullptr);
        ::execv(ENTROPIC_CLI_BIN_PATH, argv.data());
        ::_exit(127);
    }
    return pid;
}

/// @brief Wait up to `limit` for the child; its exit code, or -1 if it is
///        still running (then killed) or died by signal.
int wait_exit(pid_t pid, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    return -1;
}

/// @brief Wait up to `limit` for a file to appear with content.
bool await_file(const fs::path& p, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!slurp(p).empty()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

SCENARIO("gh#204: entropic serve hosts the engine for a client holding the token",
         "[serve][cli][gh204][2.14.0]") {
    ServeScratch s;
    const int port = free_port();
    const fs::path token_file =
        s.home / ".entropic" / "serve" / (std::to_string(port) + ".token");
    const pid_t pid = spawn_serve(
        s, {"--config", s.config.string(), "--port", std::to_string(port)});
    REQUIRE(pid > 0);
    const bool up = await_file(token_file, std::chrono::seconds(30));
    INFO("serve stderr:\n" << slurp(s.stderr_log));
    if (!up) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    REQUIRE(up);
    const std::string token = slurp(token_file);

    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(10, 0);
    auto r = cli.Post("/mcp", {{"Authorization", "Bearer " + token}},
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})",
        "application/json");
    auto anon = cli.Post("/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})",
        "application/json");
    ::kill(pid, SIGTERM);
    const int code = wait_exit(pid, std::chrono::seconds(30));

    THEN("the token holder is served the bridge's tools; nobody else is") {
        REQUIRE(r);
        REQUIRE(r->status == 200);
        INFO("body: " << r->body);
        bool has_ask = false;
        const auto reply = json::parse(r->body);
        for (const auto& t : reply["result"]["tools"]) {
            has_ask = has_ask || t["name"] == "entropic.ask";
        }
        CHECK(has_ask);
        REQUIRE(anon);
        CHECK(anon->status == 401);
    }
    THEN("SIGTERM shuts it down cleanly and removes the token") {
        CHECK(code == 0);
        CHECK_FALSE(fs::exists(token_file));
        CHECK(slurp(s.stderr_log).find("http://127.0.0.1:" + std::to_string(port) + "/mcp")
              != std::string::npos);
    }
}

SCENARIO("v2.14.1: SIGUSR1 releases models and keeps serving",
         "[serve][cli][2.14.1]") {
    // sumac frees the card for a GPU job with SIGUSR1 on its own host; a
    // serve host had to be stopped instead, costing every conversation and
    // the token. SIGUSR1 now calls entropic_release_model and the loop
    // goes back to waiting.
    ServeScratch s;
    const int port = free_port();
    const fs::path token_file =
        s.home / ".entropic" / "serve" / (std::to_string(port) + ".token");
    const pid_t pid = spawn_serve(
        s, {"--config", s.config.string(), "--port", std::to_string(port)});
    REQUIRE(pid > 0);
    const bool up = await_file(token_file, std::chrono::seconds(30));
    if (!up) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    REQUIRE(up);
    const std::string token = slurp(token_file);

    ::kill(pid, SIGUSR1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (slurp(s.stderr_log).find("models released") == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(10, 0);
    auto r = cli.Post("/mcp", {{"Authorization", "Bearer " + token}},
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})",
        "application/json");
    const bool token_kept = slurp(token_file) == token;
    ::kill(pid, SIGTERM);
    const int code = wait_exit(pid, std::chrono::seconds(30));
    INFO("serve stderr:\n" << slurp(s.stderr_log));

    THEN("the release is reported, and the host keeps serving with the same token") {
        CHECK(slurp(s.stderr_log).find("SIGUSR1, models released (0)") != std::string::npos);
        REQUIRE(r);
        CHECK(r->status == 200);
        CHECK(token_kept);
    }
    THEN("SIGTERM still ends it cleanly") {
        CHECK(code == 0);
        CHECK_FALSE(fs::exists(token_file));
    }
}

SCENARIO("gh#204: entropic serve refuses to look up when it is not",
         "[serve][cli][gh204][2.14.0]") {
    ServeScratch s;
    WHEN("--port is missing") {
        const pid_t pid = spawn_serve(s, {"--config", s.config.string()});
        THEN("it prints usage and exits 1") {
            CHECK(wait_exit(pid, std::chrono::seconds(10)) == 1);
            CHECK(slurp(s.stderr_log).find("usage: entropic serve")
                  != std::string::npos);
        }
    }
    WHEN("the port is not a number") {
        const pid_t pid = spawn_serve(
            s, {"--config", s.config.string(), "--port", "not-a-port"});
        THEN("configure fails naming the listener, and it exits 1") {
            CHECK(wait_exit(pid, std::chrono::seconds(30)) == 1);
            const auto err = slurp(s.stderr_log);
            INFO(err);
            CHECK(err.find("cannot start") != std::string::npos);
            CHECK(err.find("HTTP listener") != std::string::npos);
        }
    }
    WHEN("the port is already taken") {
        const int busy = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(busy, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(busy, 1);
        socklen_t len = sizeof(a);
        ::getsockname(busy, reinterpret_cast<sockaddr*>(&a), &len);
        const pid_t pid = spawn_serve(
            s, {"--config", s.config.string(),
                "--port", std::to_string(ntohs(a.sin_port))});
        const int code = wait_exit(pid, std::chrono::seconds(30));
        ::close(busy);
        THEN("it exits 1 instead of serving nothing") {
            INFO(slurp(s.stderr_log));
            CHECK(code == 1);
        }
    }
}
