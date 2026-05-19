// SPDX-License-Identifier: Apache-2.0
/**
 * @file mcp_bridge_relay_test.cpp
 * @brief End-to-end relay tests for the `entropic mcp-bridge` CLI (v2.1.7,
 *        gh#34).
 *
 * Forks the entropic binary with `--socket PATH` and exchanges JSON-RPC
 * lines through a fake unix-socket server in the parent process. Verifies:
 *   1. mcp-bridge exits non-zero with a diagnostic when no engine is
 *      reachable.
 *   2. A line written to mcp-bridge's stdin reaches the socket peer
 *      byte-for-byte.
 *   3. A line written by the socket peer reaches mcp-bridge's stdout
 *      byte-for-byte.
 *   4. EOF on stdin half-closes the socket write side so the engine
 *      observes a clean disconnect.
 *
 * @version 2.1.7
 */

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief Create a listening AF_UNIX socket at the given path.
 * @param path Socket path (deleted if already present).
 * @return Listening fd, or -1 on failure.
 * @utility
 * @version 2.1.7
 */
int make_listener(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
     || ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Read a single newline-terminated line from fd.
 * @utility
 */
std::string read_line(int fd) {
    std::string out;
    char c = 0;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) { return out; }
        if (c == '\n') { return out; }
        out += c;
    }
}

/**
 * @brief Fork the entropic binary as a child with piped stdio.
 *
 * @param args        argv to pass after the binary path.
 * @param stdin_w_out Output: parent-side write end of child's stdin pipe.
 * @param stdout_r_out Output: parent-side read end of child's stdout pipe.
 * @return Child pid, or -1 on failure.
 * @utility
 * @version 2.1.7
 */
void child_exec_bridge(int stdin_r, int stdout_w, int stdin_w, int stdout_r,
                       const std::vector<std::string>& args) {
    ::dup2(stdin_r, STDIN_FILENO);
    ::dup2(stdout_w, STDOUT_FILENO);
    ::close(stdin_r); ::close(stdin_w);
    ::close(stdout_w); ::close(stdout_r);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(ENTROPIC_CLI_BIN_PATH));
    argv.push_back(const_cast<char*>("mcp-bridge"));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(ENTROPIC_CLI_BIN_PATH, argv.data());
    std::_Exit(127);
}

pid_t spawn_bridge(const std::vector<std::string>& args,
                   int& stdin_w_out, int& stdout_r_out) {
    int sp[2];
    int op[2];
    bool ok = (::pipe(sp) == 0) && (::pipe(op) == 0);
    pid_t pid = ok ? ::fork() : -1;
    if (pid == 0) {
        child_exec_bridge(sp[0], op[1], sp[1], op[0], args);
    }
    if (pid > 0) {
        ::close(sp[0]); ::close(op[1]);
        stdin_w_out = sp[1];
        stdout_r_out = op[0];
    }
    return pid;
}

}  // namespace

SCENARIO("mcp-bridge exits non-zero with diagnostic when no engine",
         "[mcp_bridge][cli][2.1.7][gh34]")
{
    GIVEN("a socket path that does not exist") {
        std::string sock = "/tmp/test-gh34-noengine-" +
            std::to_string(::getpid()) + ".sock";
        ::unlink(sock.c_str());

        int stdin_w = -1, stdout_r = -1;
        pid_t pid = spawn_bridge({"--socket", sock}, stdin_w, stdout_r);
        REQUIRE(pid > 0);

        WHEN("we wait for the child") {
            int status = 0;
            ::waitpid(pid, &status, 0);
            THEN("it exits with status 1") {
                REQUIRE(WIFEXITED(status));
                CHECK(WEXITSTATUS(status) == 1);
            }
        }
        if (stdin_w >= 0) { ::close(stdin_w); }
        if (stdout_r >= 0) { ::close(stdout_r); }
    }
}

SCENARIO("mcp-bridge relays bytes between stdio and the socket",
         "[mcp_bridge][cli][2.1.7][gh34]")
{
    GIVEN("a listening socket and the bridge spawned against it") {
        std::string sock = "/tmp/test-gh34-relay-" +
            std::to_string(::getpid()) + ".sock";
        int listen_fd = make_listener(sock);
        REQUIRE(listen_fd >= 0);

        int stdin_w = -1, stdout_r = -1;
        pid_t pid = spawn_bridge({"--socket", sock}, stdin_w, stdout_r);
        REQUIRE(pid > 0);

        // Accept the bridge's connection.
        int peer = ::accept(listen_fd, nullptr, nullptr);
        REQUIRE(peer >= 0);

        WHEN("the client writes a request line to bridge stdin") {
            std::string req =
                R"({"jsonrpc":"2.0","id":1,"method":"initialize"})" "\n";
            ssize_t w = ::write(stdin_w, req.c_str(), req.size());
            REQUIRE(w == static_cast<ssize_t>(req.size()));

            THEN("the socket peer receives it byte-for-byte") {
                auto got = read_line(peer);
                CHECK(got + "\n" == req);
            }
        }

        WHEN("the socket peer writes a response line") {
            std::string resp =
                R"({"jsonrpc":"2.0","id":1,"result":{}})" "\n";
            ssize_t w = ::write(peer, resp.c_str(), resp.size());
            REQUIRE(w == static_cast<ssize_t>(resp.size()));

            THEN("the bridge writes it to its stdout") {
                auto got = read_line(stdout_r);
                CHECK(got + "\n" == resp);
            }
        }

        WHEN("stdin is closed (client went away)") {
            ::close(stdin_w);
            stdin_w = -1;

            THEN("the socket peer observes EOF on its read side") {
                char c = 0;
                ssize_t n = ::read(peer, &c, 1);
                CHECK(n == 0);  // clean EOF, not error
            }
        }

        // Cleanup
        if (peer >= 0) { ::close(peer); }
        if (stdin_w >= 0) { ::close(stdin_w); }
        if (stdout_r >= 0) { ::close(stdout_r); }
        ::close(listen_fd);
        ::unlink(sock.c_str());

        // Reap the bridge.
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
}
