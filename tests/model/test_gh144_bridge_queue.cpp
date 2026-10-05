// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh144_bridge_queue.cpp
 * @brief gh#144: several MCP clients on one engine QUEUE, they do not race.
 *
 * The end-to-end proof of the behaviour the consumer actually depends on.
 * Everything below runs over a REAL unix socket with a REAL engine and three
 * concurrent client connections — the thread-per-client accept path that made
 * the original race reachable in the first place. Driving
 * ExternalBridge::dispatch directly from threads would skip exactly the layer
 * under test.
 *
 * @par What was wrong
 * `@threadsafety Serialized per-handle.` on the six run entry points has been
 * false since gh#109 removed api_mutex from them, and the bridge has served
 * each client on its own thread since v2.1.2. Two concurrent asks therefore
 * raced the shared conversation AND decoded concurrently on one
 * llama_context. The consumer read the header contract and designed against
 * it.
 *
 * @par The four properties
 *   1. every client completes — none is refused
 *   2. NONE receives ALREADY_RUNNING. The engine refuses a concurrent run;
 *      the bridge queues above it so that refusal is never reached. A host
 *      wants callers to WAIT, which is the whole point of one resident model.
 *   3. sessions stay disjoint under real concurrency
 *   4. the status tool remains answerable while a turn is in flight — the
 *      gh#109 tripwire at the bridge layer. handle_clear -> cancel ->
 *      interrupt must NOT wait behind the turn it is cancelling, so nothing
 *      outside dispatch_ask may take turn_mutex_.
 *
 * Requires: GPU + gemma4_e2b QAT trunk. Run: ctest -L model -R gh144-bridge
 *
 * @version 2.12.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/mcp/external_bridge.h>
#include <entropic/types/config.h>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

/// @brief Write a file, creating parents.
/// @utility
/// @version 2.12.0
void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

/// @brief Connect to the bridge socket, retrying while it comes up.
/// @utility
/// @version 2.12.0
int connect_client(const std::string& sock_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    for (int i = 0; i < 100; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(fd);
    return -1;
}

/// @brief Read one newline-delimited JSON-RPC frame.
/// @utility
/// @version 2.12.0
std::string read_line(int fd, int timeout_s) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(timeout_s);
    std::string out;
    while (std::chrono::steady_clock::now() < deadline) {
        char c = 0;
        ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT);
        if (n == 1) {
            if (c == '\n') { return out; }
            out.push_back(c);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return out;
}

/// @brief Send one JSON-RPC line.
/// @utility
/// @version 2.12.0
void send_line(int fd, const json& j) {
    auto s = j.dump() + "\n";
    ssize_t rc = ::write(fd, s.c_str(), s.size());
    (void)rc;
}

/// @brief One client's ask, skipping any progress notifications.
/// @utility
/// @version 2.12.0
std::string ask(int fd, int id, const std::string& session,
                const std::string& prompt) {
    send_line(fd, {{"jsonrpc", "2.0"}, {"id", id},
                   {"method", "tools/call"},
                   {"params", {{"name", "entropic.ask"},
                               {"arguments", {{"prompt", prompt},
                                              {"session", session}}}}}});
    for (int i = 0; i < 400; ++i) {
        // Generous: the LAST client in the queue waits for every turn
        // ahead of it plus its own. Being impatient here reports a working
        // queue as a failure.
        auto line = read_line(fd, 300);
        if (line.empty()) { break; }
        auto j = json::parse(line, nullptr, false);
        if (j.is_discarded()) { continue; }
        // Skip notifications/progress frames; take the response with our id.
        if (j.contains("id") && j["id"] == id) { return line; }
    }
    return "";
}

}  // namespace

SCENARIO("gh#144: concurrent bridge clients queue instead of racing",
         "[model][gh144][bridge]")
{
    GIVEN("one engine serving three concurrent clients over a real socket") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path trunk = fs::path(home) / ".entropic" / "models"
                         / "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf";
        REQUIRE(fs::is_regular_file(trunk));

        auto sock = std::string("/tmp/entropic-gh144-queue-")
                  + std::to_string(static_cast<long>(::getpid())) + ".sock";
        fs::remove(sock);

        fs::path dir = fs::temp_directory_path() / "entropic_gh144_queue";
        fs::remove_all(dir);
        write_file(dir / "config.local.yaml",
                   "models:\n"
                   "  lead:\n"
                   "    path: " + trunk.string() + "\n"
                   "    adapter: gemma4\n"
                   "    context_length: 2048\n"
                   "    gpu_layers: 99\n"
                   "    max_sessions: 3\n"
                   "    enable_thinking: false\n"
                   "  default: lead\n"
                   "generation:\n"
                   "  stream_output: false\n"
                   // Keep each turn cheap. This test measures QUEUEING, not
                   // generation quality, and the third client waits behind
                   // two full turns before its own — with an unbounded
                   // budget and <|channel> thinking runaway that wait
                   // exceeded the client timeout and looked like a failure.
                   "  max_tokens: 16\n"
                   "mcp:\n"
                   "  working_dir: " + dir.string() + "\n"  // gh#186
                   "  external:\n"
                   "    enabled: true\n"
                   "    ask_streaming: false\n"
                   "    socket_path: " + sock + "\n"
                   "constitutional_validation:\n"
                   "  enabled: false\n");

        setenv("ENTROPIC_DATA_DIR",
               (fs::path(MODEL_PATH) / "data").string().c_str(), 1);

        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        REQUIRE(entropic_configure_dir(h, dir.string().c_str())
                == ENTROPIC_OK);

        WHEN("three clients ask simultaneously on distinct sessions") {
            std::vector<std::string> replies(3);
            std::atomic<bool> go{false};
            std::atomic<int> status_replies{0};

            std::vector<std::thread> clients;
            for (int i = 0; i < 3; ++i) {
                clients.emplace_back([&, i] {
                    int fd = connect_client(sock);
                    if (fd < 0) { return; }
                    while (!go.load()) {
                        std::this_thread::yield();
                    }
                    replies[static_cast<std::size_t>(i)] = ask(
                        fd, 100 + i, "repo-" + std::to_string(i),
                        "Reply with exactly one short sentence.");
                    ::close(fd);
                });
            }

            // A fourth connection polls status while the others are busy.
            std::thread watcher([&] {
                int fd = connect_client(sock);
                if (fd < 0) { return; }
                while (!go.load()) { std::this_thread::yield(); }
                for (int i = 0; i < 60; ++i) {
                    send_line(fd, {{"jsonrpc", "2.0"}, {"id", 900 + i},
                                   {"method", "tools/call"},
                                   {"params", {{"name", "entropic.status"},
                                               {"arguments", json::object()}}}});
                    auto line = read_line(fd, 5);
                    if (line.find("queue_depth:") != std::string::npos) {
                        status_replies.fetch_add(1);
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50));
                }
                ::close(fd);
            });

            go.store(true);
            for (auto& t : clients) { t.join(); }
            watcher.join();

            // ONE leaf section on purpose. Catch2 re-executes the whole
            // GIVEN/WHEN path once per leaf, and this GIVEN creates an
            // engine, loads a model and BINDS A SOCKET — so four AND_THENs
            // meant four bridges racing for the same path, which is what
            // made this test flaky (1 in 3) before. The flake was my test
            // design, not an engine race; all the checks belong in one leaf.
            THEN("all three queued, none was refused, sessions stayed apart") {
                for (std::size_t i = 0; i < replies.size(); ++i) {
                    INFO("client " << i << " reply: " << replies[i]);
                    REQUIRE_FALSE(replies[i].empty());
                }

                // The engine refuses a concurrent run; the bridge queues
                // above it so that refusal is never reached from here. A
                // host wants callers to WAIT — the point of one resident
                // model.
                for (std::size_t i = 0; i < replies.size(); ++i) {
                    INFO("client " << i << " reply: " << replies[i]);
                    CHECK(replies[i].find("already") == std::string::npos);
                    CHECK(replies[i].find("ALREADY_RUNNING")
                          == std::string::npos);
                }

                // The gh#109 tripwire at the bridge layer: nothing outside
                // dispatch_ask may take turn_mutex_, or status would block
                // behind the very turn it needs to observe. An earlier cut
                // asserted `>= 0`, true of any counter and proof of nothing.
                INFO("status replies observed: " << status_replies.load());
                CHECK(status_replies.load() > 0);

                for (int i = 0; i < 3; ++i) {
                    size_t n = 0;
                    auto key = "repo-" + std::to_string(i);
                    REQUIRE(entropic_session_context_count(
                                h, key.c_str(), &n) == ENTROPIC_OK);
                    INFO("session " << key << " holds " << n << " messages");
                    CHECK(n > 0);
                }
                size_t def = 0;
                REQUIRE(entropic_session_context_count(h, "", &def)
                        == ENTROPIC_OK);
                CHECK(def == 0);
            }
        }

        entropic_destroy(h);
        fs::remove(sock);
    }
}
