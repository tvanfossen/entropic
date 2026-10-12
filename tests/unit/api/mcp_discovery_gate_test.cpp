// SPDX-License-Identifier: Apache-2.0
/**
 * @file mcp_discovery_gate_test.cpp
 * @brief v2.14.0 (gh#199, gh#200): external MCP servers connect only when
 *        an operator ENABLED them — through the handle's real C ABI.
 *
 * Observed with a SPAWN MARKER rather than a connection: each `.mcp.json`
 * or YAML entry under test is `/bin/sh -c "touch <marker>"`. It is not an
 * MCP server, so the handshake fails either way; what the test reads is
 * whether the engine LAUNCHED the process at all, which is the hazard the
 * issues describe (a consumer's workspace spawned a 58-tool application
 * server and four nested launchers it never asked for).
 *
 * The policy is Claude Code's: discover, then connect only what is enabled.
 * `mcp.disabled_mcpjson_servers` always refuses; `enabled_mcpjson_servers`
 * or `enable_all_project_mcp_servers` pre-approve; otherwise the
 * `ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT` pre-hook decides; with nobody to
 * ask, nothing connects.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

/// @brief Owns one configured handle.
struct GateHandle {
    entropic_handle_t h = nullptr;  ///< Handle under test
    bool ok = false;                ///< Whether configure succeeded
    explicit GateHandle(const std::string& cfg) {
        entropic_create(&h);
        if (h != nullptr) { ok = entropic_configure(h, cfg.c_str()) == ENTROPIC_OK; }
    }
    ~GateHandle() { entropic_destroy(h); }
    GateHandle(const GateHandle&) = delete;
    GateHandle& operator=(const GateHandle&) = delete;
};

/// @brief A fresh directory under /tmp for one scenario.
fs::path scratch(const std::string& tag) {
    auto dir = fs::temp_directory_path()
        / ("entropic_gate_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// @brief JSON for a stdio entry that only touches `marker` when launched.
std::string probe_entry(const fs::path& marker) {
    return R"({"command":"/bin/sh","args":["-c","touch ')"
         + marker.string() + R"('"]})";
}

/// @brief Write `<dir>/.mcp.json` declaring one probe server named `name`.
void write_mcp_json(const fs::path& dir, const std::string& name,
                    const fs::path& marker) {
    std::ofstream(dir / ".mcp.json")
        << R"({"mcpServers":{")" << name << R"(":)" << probe_entry(marker)
        << "}}";
}

/// @brief Whether `marker` appears within `wait` — i.e. whether it ran.
bool spawned(const fs::path& marker,
             std::chrono::milliseconds wait = std::chrono::milliseconds(1500)) {
    const auto until = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < until) {
        if (fs::exists(marker)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return fs::exists(marker);
}

/// @brief Handle config rooted at `root`, with extra `mcp` keys spliced in.
std::string cfg_rooted(const fs::path& root, const std::string& mcp_extra = "") {
    std::string mcp = R"("working_dir":")" + root.string() + R"(")";
    if (!mcp_extra.empty()) { mcp += "," + mcp_extra; }
    return R"({"log_level":"WARN","mcp":{)" + mcp + "}}";
}

}  // namespace

SCENARIO("gh#199: a workspace does not spawn what its root's .mcp.json declares",
         "[api][mcp][discovery][gh199][2.14.0]") {
    GIVEN("a repository whose .mcp.json declares a server, and a host that "
          "enabled nothing") {
        const auto host = scratch("ws_host");
        const auto repo = scratch("ws_repo");
        const auto marker = repo / "SPAWNED";
        write_mcp_json(repo, "probe", marker);
        GateHandle h(cfg_rooted(host));
        REQUIRE(h.ok);

        WHEN("a workspace is created on that repository") {
            REQUIRE(entropic_workspace_create(h.h, "repo",
                                              repo.string().c_str())
                    == ENTROPIC_OK);
            THEN("the declared server is never launched") {
                // Pre-2.14.0 this spawned unconditionally — the consumer saw
                // `Spawned child process for 'sassafras'` from a handle
                // configured by file precisely to avoid project config.
                CHECK_FALSE(spawned(marker, std::chrono::milliseconds(400)));
            }
        }
    }

    GIVEN("the same repository, and a host that enabled that server by name") {
        const auto host = scratch("ws_host2");
        const auto repo = scratch("ws_repo2");
        const auto marker = repo / "SPAWNED";
        write_mcp_json(repo, "probe", marker);
        GateHandle h(cfg_rooted(host, R"("enabled_mcpjson_servers":["probe"])"));
        REQUIRE(h.ok);

        WHEN("a workspace is created on it") {
            REQUIRE(entropic_workspace_create(h.h, "repo",
                                              repo.string().c_str())
                    == ENTROPIC_OK);
            THEN("the enabled server is launched") {
                CHECK(spawned(marker));
            }
        }
    }
}

SCENARIO("v2.14.0: the main handle discovers too, and the same gate applies",
         "[api][mcp][discovery][2.14.0]") {
    GIVEN("a root whose .mcp.json declares a server") {
        const auto root = scratch("main_root");
        const auto marker = root / "SPAWNED";
        write_mcp_json(root, "probe", marker);

        WHEN("the handle enabled nothing") {
            GateHandle h(cfg_rooted(root));
            REQUIRE(h.ok);
            THEN("nothing is launched") {
                CHECK_FALSE(spawned(marker, std::chrono::milliseconds(400)));
            }
        }
        WHEN("the handle enabled it by name") {
            GateHandle h(cfg_rooted(root, R"("enabled_mcpjson_servers":["probe"])"));
            REQUIRE(h.ok);
            THEN("it is launched") {
                // Pre-2.14.0 the main handle never connected discovered
                // servers at all: nothing called ServerManager::initialize().
                CHECK(spawned(marker));
            }
        }
        WHEN("enable-all is on but the server is also disabled by name") {
            GateHandle h(cfg_rooted(root,
                R"("enable_all_project_mcp_servers":true,)"
                R"("disabled_mcpjson_servers":["probe"])"));
            REQUIRE(h.ok);
            THEN("the explicit refusal wins") {
                CHECK_FALSE(spawned(marker, std::chrono::milliseconds(400)));
            }
        }
    }
}

SCENARIO("v2.14.0: a server declared in the handle's own config connects",
         "[api][mcp][external_servers][2.14.0]") {
    GIVEN("mcp.external_servers declaring one stdio server") {
        const auto root = scratch("yaml_root");
        const auto marker = root / "SPAWNED";
        const std::string extra =
            R"("external_servers":{"probe":)" + probe_entry(marker) + "}";
        GateHandle h(cfg_rooted(root, extra));
        REQUIRE(h.ok);

        THEN("it is launched without any enablement — the operator declared it") {
            // Pre-2.14.0 this key was in the struct and the schema but never
            // parsed from config, and nothing would have connected it anyway.
            CHECK(spawned(marker));
        }
    }
}

namespace {

/// @brief Pre-hook that returns a fixed verdict and counts its calls.
struct VerdictHook {
    int rc = 0;       ///< 0 = connect, non-zero = refuse
    int calls = 0;    ///< How many times it was asked
    std::string last; ///< Last context JSON it saw

    static int call(entropic_hook_point_t, const char* ctx, char**, void* ud) {
        auto* self = static_cast<VerdictHook*>(ud);
        ++self->calls;
        self->last = ctx != nullptr ? ctx : "";
        return self->rc;
    }
};

}  // namespace

SCENARIO("v2.14.0: the pre-connect hook decides what config left open",
         "[api][mcp][discovery][hook][gh199][2.14.0]") {
    GIVEN("a repository declaring a server, and a host that enabled nothing") {
        const auto host = scratch("hook_host");
        const auto repo = scratch("hook_repo");
        const auto marker = repo / "SPAWNED";
        write_mcp_json(repo, "probe", marker);
        GateHandle h(cfg_rooted(host));
        REQUIRE(h.ok);
        VerdictHook hook;

        WHEN("the hook approves") {
            hook.rc = 0;
            REQUIRE(entropic_register_hook(h.h,
                        ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT,
                        &VerdictHook::call, &hook, 0) == ENTROPIC_OK);
            REQUIRE(entropic_workspace_create(h.h, "repo",
                                              repo.string().c_str())
                    == ENTROPIC_OK);
            THEN("it was asked about that server, and the server launched") {
                CHECK(hook.calls == 1);
                CHECK(hook.last.find("\"probe\"") != std::string::npos);
                CHECK(spawned(marker));
            }
        }
        WHEN("the hook refuses") {
            hook.rc = 1;
            REQUIRE(entropic_register_hook(h.h,
                        ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT,
                        &VerdictHook::call, &hook, 0) == ENTROPIC_OK);
            REQUIRE(entropic_workspace_create(h.h, "repo",
                                              repo.string().c_str())
                    == ENTROPIC_OK);
            THEN("it was asked, and nothing launched") {
                CHECK(hook.calls == 1);
                CHECK_FALSE(spawned(marker, std::chrono::milliseconds(400)));
            }
        }
    }

    GIVEN("a server config has already disabled") {
        const auto host = scratch("hook_host2");
        const auto repo = scratch("hook_repo2");
        const auto marker = repo / "SPAWNED";
        write_mcp_json(repo, "probe", marker);
        GateHandle h(cfg_rooted(host, R"("disabled_mcpjson_servers":["probe"])"));
        REQUIRE(h.ok);
        VerdictHook hook;   // approves
        REQUIRE(entropic_register_hook(h.h,
                    ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT,
                    &VerdictHook::call, &hook, 0) == ENTROPIC_OK);

        WHEN("a workspace is created on it") {
            REQUIRE(entropic_workspace_create(h.h, "repo",
                                              repo.string().c_str())
                    == ENTROPIC_OK);
            THEN("the hook is never asked and nothing launches") {
                // Config refusals are final; a permissive hook cannot
                // override an operator's explicit no.
                CHECK(hook.calls == 0);
                CHECK_FALSE(spawned(marker, std::chrono::milliseconds(400)));
            }
        }
    }
}

SCENARIO("gh#175: a non-positive mcp.server_timeout_seconds is refused at configure",
         "[api][mcp][gh175][2.14.0]") {
    GIVEN("server_timeout_seconds set to 0") {
        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        const auto rc = entropic_configure(
            h, R"({"log_level":"WARN","mcp":{"server_timeout_seconds":0}})");
        THEN("configure fails loudly instead of bounding nothing") {
            CHECK(rc != ENTROPIC_OK);
        }
        entropic_destroy(h);
    }
}
