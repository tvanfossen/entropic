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
    BridgeHandle() {
        dir = fs::temp_directory_path() / ("entropic_release_tool_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        entropic_create(&h);
        const std::string cfg = R"({"log_level":"WARN","mcp":{"working_dir":")" + dir.string()
            + R"(","external":{"enabled":true,"socket_path":")" + (dir / "b.sock").string()
            + R"("}}})";
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
