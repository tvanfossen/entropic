// SPDX-License-Identifier: Apache-2.0
/**
 * @file discovery_policy_test.cpp
 * @brief v2.14.0 (gh#199, gh#200): the pure external-server decisions.
 *
 * The behavioural half — what the engine actually launches — is
 * tests/unit/api/mcp_discovery_gate_test.cpp, with spawn markers. This pins
 * the two decisions that are awkward to observe that way: config precedence,
 * and where an `entropic mcp-bridge` entry would really connect.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include "../../../src/mcp/discovery_policy.h"

#include <filesystem>
#include <unistd.h>

using entropic::ExternalServerConfig;
using entropic::MCPConfig;
using entropic::McpJsonConfigVerdict;
using entropic::launches_own_bridge;
using entropic::mcpjson_config_verdict;

namespace {

namespace fs = std::filesystem;

/// @brief A stdio entry running `command args...`.
ExternalServerConfig stdio(const std::string& command,
                           std::vector<std::string> args,
                           const std::string& cwd = "") {
    ExternalServerConfig c;
    c.name = "x";
    c.transport = "stdio";
    c.command = command;
    c.args = std::move(args);
    c.working_dir = cwd;
    return c;
}

/// @brief A real directory, so weakly_canonical resolves it the same way
///        the bridge does.
fs::path dir(const std::string& tag) {
    auto d = fs::temp_directory_path()
        / ("entropic_dp_" + tag + "_" + std::to_string(::getpid()));
    fs::create_directories(d);
    return d;
}

}  // namespace

SCENARIO("config decides a discovered server before any hook is asked",
         "[mcp][discovery][gh199][2.14.0]") {
    MCPConfig cfg;
    GIVEN("nothing enabled or disabled") {
        THEN("config is silent and the hook decides") {
            CHECK(mcpjson_config_verdict("probe", cfg) == McpJsonConfigVerdict::ask);
        }
    }
    GIVEN("the server enabled by name") {
        cfg.enabled_mcpjson_servers = {"probe"};
        THEN("it is enabled, and only it") {
            CHECK(mcpjson_config_verdict("probe", cfg) == McpJsonConfigVerdict::enabled);
            CHECK(mcpjson_config_verdict("other", cfg) == McpJsonConfigVerdict::ask);
        }
    }
    GIVEN("enable-all") {
        cfg.enable_all_project_mcp_servers = true;
        THEN("every name is enabled") {
            CHECK(mcpjson_config_verdict("anything", cfg) == McpJsonConfigVerdict::enabled);
        }
        AND_GIVEN("one server also disabled by name") {
            cfg.disabled_mcpjson_servers = {"probe"};
            THEN("the explicit refusal beats enable-all") {
                CHECK(mcpjson_config_verdict("probe", cfg) == McpJsonConfigVerdict::disabled);
            }
        }
    }
    GIVEN("a name both enabled and disabled") {
        cfg.enabled_mcpjson_servers = {"probe"};
        cfg.disabled_mcpjson_servers = {"probe"};
        THEN("disabled wins") {
            CHECK(mcpjson_config_verdict("probe", cfg) == McpJsonConfigVerdict::disabled);
        }
    }
}

SCENARIO("an entropic mcp-bridge entry is recognised wherever it would connect",
         "[mcp][discovery][gh200][2.14.0]") {
    const auto root = dir("root");
    const auto own = entropic::compute_socket_path(fs::weakly_canonical(root));

    GIVEN("this handle's bridge binds the socket derived from `root`") {
        WHEN("an entry runs the bridge with no arguments in `root`") {
            // Our own tracked .mcp.json, exactly: the child gets the set's
            // root as cwd and derives the same socket. The pre-2.14.0 guard
            // compared only a non-standard "path" key and could not see this.
            auto c = stdio("entropic", {"mcp-bridge"});
            THEN("it is our own bridge") {
                CHECK(launches_own_bridge(c, own, root));
            }
        }
        WHEN("an entry names our socket explicitly") {
            auto c = stdio("/usr/local/bin/entropic",
                           {"mcp-bridge", "--socket", own.string()});
            THEN("it is our own bridge") {
                CHECK(launches_own_bridge(c, own, root));
            }
        }
        WHEN("an entry points the bridge at a different project") {
            const auto other = dir("other");
            auto c = stdio("entropic", {"mcp-bridge", "--project-dir",
                                        other.string()});
            THEN("it is someone else's engine, not ours") {
                CHECK_FALSE(launches_own_bridge(c, own, root));
            }
        }
        WHEN("the entry is not the entropic bridge at all") {
            auto c = stdio("/bin/sh", {"-c", "mcp-bridge"});
            THEN("it is not ours") {
                CHECK_FALSE(launches_own_bridge(c, own, root));
            }
        }
    }
    GIVEN("a handle that runs no bridge") {
        auto c = stdio("entropic", {"mcp-bridge"});
        THEN("nothing can connect back to it") {
            CHECK_FALSE(launches_own_bridge(c, fs::path{}, root));
        }
    }
}
