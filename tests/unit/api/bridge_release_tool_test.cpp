// SPDX-License-Identifier: Apache-2.0
/**
 * @file bridge_release_tool_test.cpp
 * @brief v2.14.3: `<prefix>.release` gives the card back over MCP.
 *
 * Through v2.14.2 an MCP client could load a model (any ask) but had no way
 * to unload one: the bridge's five tools are ask, ask_status, status,
 * context_clear and context_count, so freeing device memory needed a shell
 * on the host (SIGUSR1/SIGTERM to `entropic serve`). `release` calls
 * entropic_release_model(h, NULL) — every resident model unloaded; the
 * handle, its sessions and their conversations kept; the next ask reloads.
 *
 * Contract (from the consumer request):
 *   - advertised with the other tools, under the configured prefix;
 *   - says what it did: which models it released, or that none was resident;
 *   - a turn in flight is a retryable tool ERROR, not a queued wait.
 *
 * @version 2.14.3
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: external_bridge, engine

#include <nlohmann/json.hpp>

#include <csignal>
#include <ctime>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// @brief A configured handle (no models) with the external bridge enabled.
struct BridgeHandle {
    entropic_handle_t h = nullptr;
    fs::path dir;
    explicit BridgeHandle(const std::string& external_extra = "") {
        dir = fs::temp_directory_path() / ("entropic_release_tool_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        entropic_create(&h);
        const std::string cfg = R"({"log_level":"WARN","mcp":{"working_dir":")" + dir.string()
            + R"(","external":{"enabled":true,"socket_path":")" + (dir / "b.sock").string()
            + "\"" + external_extra + "}}}";
        REQUIRE(entropic_configure(h, cfg.c_str()) == ENTROPIC_OK);
        REQUIRE(h->external_bridge != nullptr);
    }
    ~BridgeHandle() { entropic_destroy(h); std::error_code ec; fs::remove_all(dir, ec); }
    BridgeHandle(const BridgeHandle&) = delete;
    BridgeHandle& operator=(const BridgeHandle&) = delete;

    json call(const std::string& method, const json& params = json::object()) {
        json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
        return json::parse(h->external_bridge->dispatch(req.dump(), -1));
    }
};

/// @brief A tools/call result's text.
std::string text_of(const json& reply) {
    std::string out;
    for (const auto& c : reply["result"].value("content", json::array())) {
        out += c.value("text", "");
    }
    return out;
}

}  // namespace

SCENARIO("v2.14.3: the bridge offers a release tool that frees the models",
         "[api][bridge][release][2.14.3]") {
    BridgeHandle b;

    THEN("tools/list advertises it under the configured prefix, taking no arguments") {
        bool found = false;
        const auto listed = b.call("tools/list");  // bind: never iterate a temporary's member
        for (const auto& t : listed["result"]["tools"]) {
            if (t["name"] == "entropic.release") {
                found = true;
                CHECK(t["inputSchema"]["properties"].empty());
            }
        }
        CHECK(found);
    }
    WHEN("it is called with nothing resident") {
        auto reply = b.call("tools/call", {{"name", "entropic.release"},
                                           {"arguments", json::object()}});
        THEN("it succeeds and says nothing was resident") {
            INFO(reply.dump());
            CHECK_FALSE(reply["result"].value("isError", false));
            CHECK(text_of(reply).find("nothing was resident") != std::string::npos);
        }
    }
    WHEN("it is called while a turn is in flight") {
        REQUIRE(b.h->engine->try_begin_turn(""));
        auto reply = b.call("tools/call", {{"name", "entropic.release"},
                                           {"arguments", json::object()}});
        b.h->engine->end_turn("");
        THEN("it refuses as a tool error the caller can retry, instead of waiting") {
            INFO(reply.dump());
            CHECK(reply["result"].value("isError", false));
            CHECK(text_of(reply).find("retry") != std::string::npos);
        }
    }
}

SCENARIO("gh#217: release is refused while ANY session is decoding",
         "[api][bridge][release][gh217][2.14.6]") {
    // serve runs with per-session concurrency. release claimed only the
    // DEFAULT session's turn, so with session "s" mid-decode the claim
    // succeeded and the model was unloaded under the stream: CUDA error,
    // host dead. The v2.14.3 test above claimed "" — the one key the guard
    // did cover.
    BridgeHandle b;
    REQUIRE(b.h->engine->concurrent_sessions());
    WHEN("another session holds a turn") {
        REQUIRE(b.h->engine->try_begin_turn("s"));
        auto reply = b.call("tools/call", {{"name", "entropic.release"},
                                           {"arguments", json::object()}});
        const auto rc = entropic_release_model(b.h, nullptr);
        b.h->engine->end_turn("s");
        THEN("both the tool and the C API refuse") {
            INFO(reply.dump());
            CHECK(reply["result"].value("isError", false));
            CHECK(rc == ENTROPIC_ERROR_ALREADY_RUNNING);
        }
    }
    WHEN("a release holds the engine") {
        REQUIRE(b.h->engine->try_begin_turn("", /*exclusive=*/true));
        const bool other = b.h->engine->try_begin_turn("s");
        if (other) { b.h->engine->end_turn("s"); }
        b.h->engine->end_turn("");
        THEN("no session can start a turn until it is done") {
            CHECK_FALSE(other);
            REQUIRE(b.h->engine->try_begin_turn("s"));
            b.h->engine->end_turn("s");
        }
    }
}

SCENARIO("v2.14.6: shutdown stops the host over MCP, only where the host allows it",
         "[api][bridge][shutdown][2.14.6]") {
    // A consumer whose operator denies `kill` had no in-band way to stop
    // `entropic serve`. The tool exists only when the host opts in
    // (`entropic serve --allow-shutdown`): the bridge is library code, and
    // an embedder must never get a tool that signals its own process.
    auto advertised = [](BridgeHandle& b) {
        const auto listed = b.call("tools/list");  // bind: never iterate a temporary's member
        bool found = false;
        for (const auto& t : listed["result"]["tools"]) {
            found = found || t["name"] == "entropic.shutdown";
        }
        return found;
    };
    auto shutdown = [](BridgeHandle& b) {
        return b.call("tools/call", {{"name", "entropic.shutdown"},
                                     {"arguments", json::object()}});
    };
    GIVEN("a host that did not opt in") {
        BridgeHandle b;
        THEN("the tool is neither advertised nor callable") {
            CHECK_FALSE(advertised(b));
            auto reply = shutdown(b);
            INFO(reply.dump());
            CHECK(text_of(reply).find("unknown tool") != std::string::npos);
        }
    }
    GIVEN("the opt-in arriving the way `entropic serve --allow-shutdown` sends it") {
        // serve sets ENTROPIC_MCP__EXTERNAL__ALLOW_SHUTDOWN; the env
        // overrides are an explicit list, so an unmapped key does nothing.
        setenv("ENTROPIC_MCP__EXTERNAL__ALLOW_SHUTDOWN", "true", 1);
        BridgeHandle b;
        unsetenv("ENTROPIC_MCP__EXTERNAL__ALLOW_SHUTDOWN");
        THEN("the tool is advertised") {
            CHECK(advertised(b));
        }
    }
    GIVEN("a host that opted in, with another session decoding") {
        BridgeHandle b(R"(,"allow_shutdown":true)");
        REQUIRE(advertised(b));
        REQUIRE(b.h->engine->try_begin_turn("s"));
        auto reply = shutdown(b);
        b.h->engine->end_turn("s");
        THEN("it refuses as a retryable error and does not stop") {
            INFO(reply.dump());
            CHECK(reply["result"].value("isError", false));
            CHECK(text_of(reply).find("retry") != std::string::npos);
        }
    }
    GIVEN("a host that opted in and is idle") {
        // SIGTERM is what `entropic serve` waits for; catch it here instead
        // of dying of it. Blocked before the call so the stopping thread
        // inherits the mask.
        sigset_t term;
        sigemptyset(&term);
        sigaddset(&term, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &term, nullptr);
        BridgeHandle b(R"(,"allow_shutdown":true)");
        auto reply = shutdown(b);
        timespec wait{3, 0};
        const int sig = sigtimedwait(&term, nullptr, &wait);
        const bool new_turn = b.h->engine->try_begin_turn("late");
        if (new_turn) { b.h->engine->end_turn("late"); }
        pthread_sigmask(SIG_UNBLOCK, &term, nullptr);
        THEN("it replies first, then signals the host to stop, and admits no new turn") {
            INFO(reply.dump());
            CHECK_FALSE(reply["result"].value("isError", false));
            CHECK(text_of(reply).find("stopping") != std::string::npos);
            CHECK(sig == SIGTERM);
            CHECK_FALSE(new_turn);
        }
    }
}
