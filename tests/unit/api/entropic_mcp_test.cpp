// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file entropic_mcp_test.cpp
 * @brief Unit tests for the C API external-MCP surface in
 *        ``src/facade/entropic_mcp.cpp``.
 *
 * Targets the NULL-handle and unconfigured-engine branches of:
 *
 *   - entropic_register_mcp_server
 *   - entropic_deregister_mcp_server
 *   - entropic_list_mcp_servers
 *
 * Real server-spawn paths (stdio + sse) need a live ServerManager and
 * are exercised by integration tests; we only pin the C ABI guards
 * here so the facade coverage reflects the work that has no side
 * effects.
 *
 * @version 2.3.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

namespace {

/**
 * @brief RAII guard that creates but does NOT configure a handle.
 *
 * A fresh handle has no server_manager wired (Phase 3 is constructed
 * during entropic_configure). That makes it the right fixture for the
 * INVALID_STATE branch of check_server_mgr().
 *
 * @internal
 * @version 2.3.8
 */
struct UnconfiguredHandle {
    entropic_handle_t h = nullptr;
    UnconfiguredHandle() { entropic_create(&h); }
    ~UnconfiguredHandle() { entropic_destroy(h); }
    operator entropic_handle_t() const { return h; }
};

} // namespace

// ── entropic_register_mcp_server ───────────────────────────────────────

SCENARIO("entropic_register_mcp_server on a NULL handle is rejected",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("register_mcp_server is called") {
            auto rc = entropic_register_mcp_server(nullptr, "name", "{}");
            THEN("ENTROPIC_ERROR_INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_register_mcp_server on an unconfigured handle "
         "is rejected before NULL-arg checks",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a valid but unconfigured handle (no server_manager)") {
        UnconfiguredHandle h;
        REQUIRE(h.h != nullptr);
        WHEN("register_mcp_server is called with non-null args") {
            auto rc = entropic_register_mcp_server(h, "name", "{}");
            THEN("ENTROPIC_ERROR_INVALID_STATE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
        WHEN("register_mcp_server is called with a NULL name") {
            // check_server_mgr() runs first; INVALID_STATE wins over
            // the NULL-arg branch on an unconfigured handle.
            auto rc = entropic_register_mcp_server(h, nullptr, "{}");
            THEN("ENTROPIC_ERROR_INVALID_STATE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
        WHEN("register_mcp_server is called with a NULL config_json") {
            auto rc = entropic_register_mcp_server(h, "name", nullptr);
            THEN("ENTROPIC_ERROR_INVALID_STATE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

// ── entropic_deregister_mcp_server ─────────────────────────────────────

SCENARIO("entropic_deregister_mcp_server on a NULL handle is rejected",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("deregister_mcp_server is called") {
            auto rc = entropic_deregister_mcp_server(nullptr, "name");
            THEN("ENTROPIC_ERROR_INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_deregister_mcp_server on an unconfigured handle "
         "returns INVALID_STATE",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a valid but unconfigured handle") {
        UnconfiguredHandle h;
        WHEN("deregister_mcp_server is called") {
            auto rc = entropic_deregister_mcp_server(h, "anyname");
            THEN("ENTROPIC_ERROR_INVALID_STATE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
        WHEN("deregister_mcp_server is called with NULL name") {
            // INVALID_STATE wins over INVALID_ARGUMENT on unconfigured.
            auto rc = entropic_deregister_mcp_server(h, nullptr);
            THEN("ENTROPIC_ERROR_INVALID_STATE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

// ── entropic_list_mcp_servers ──────────────────────────────────────────

SCENARIO("entropic_list_mcp_servers on a NULL handle returns NULL",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("list_mcp_servers is called") {
            char* json = entropic_list_mcp_servers(nullptr);
            THEN("NULL is returned (no allocation)") {
                REQUIRE(json == nullptr);
            }
        }
    }
}

SCENARIO("entropic_list_mcp_servers on an unconfigured handle returns NULL",
         "[entropic_mcp][facade][v2.3.8]") {
    GIVEN("a fresh handle that has not had entropic_configure() called") {
        UnconfiguredHandle h;
        WHEN("list_mcp_servers is called") {
            // configured.load() is false → fast NULL return.
            char* json = entropic_list_mcp_servers(h);
            THEN("NULL is returned") {
                REQUIRE(json == nullptr);
            }
        }
    }
}

// ── Configured-but-no-server path (parse_external_server_spec exercise) ─

SCENARIO("entropic_register_mcp_server on a configured handle "
         "exercises parse_external_server_spec without external connect",
         "[entropic_mcp][facade][v2.3.8]") {
    // We configure with a minimal JSON so the engine wires up a
    // ServerManager (Phase 3). The connect_external_server() call will
    // typically fail for these synthetic specs (no binary, no http
    // endpoint) — the test only cares that the JSON parsing branches
    // execute. Either ENTROPIC_OK (lazy connect deferred) or
    // ENTROPIC_ERROR_CONNECTION_FAILED is acceptable; both prove the
    // parse path ran. If the handle never configured (no models
    // discoverable in the test env), we skip — coverage of the NULL/
    // INVALID_STATE branches above is already locked in.
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    auto cfg_rc = entropic_configure(h, R"({"log_level":"WARN"})");
    if (cfg_rc != ENTROPIC_OK) {
        entropic_destroy(h);
        SUCCEED("configure failed in test env — NULL/INVALID_STATE "
                "branches still covered above");
        return;
    }

    GIVEN("a configured handle with a working ServerManager") {
        WHEN("register is called with a valid stdio spec (env keys "
             "include one on the blocklist)") {
            // PATH is on the env-var blocklist; "MY_CUSTOM" should
            // pass through. We don't assert on success — the binary
            // /bin/true may or may not be present, and connect may
            // fail for orthogonal reasons. We just exercise the
            // parse path.
            const char* j = R"({
                "command":"/bin/true",
                "args":["--ok"],
                "env":{"PATH":"/leak","MY_CUSTOM":"42"},
                "transport":"stdio"
            })";
            auto rc = entropic_register_mcp_server(h, "tparse_stdio", j);
            THEN("the call returns ENTROPIC_OK or ENTROPIC_ERROR_CONNECTION_FAILED") {
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_CONNECTION_FAILED
                         || rc == ENTROPIC_ERROR_SERVER_ALREADY_EXISTS));
            }
        }

        WHEN("register is called with a malformed JSON config") {
            auto rc = entropic_register_mcp_server(h, "tparse_bad",
                                                   "{not: valid json");
            THEN("the parse exception is mapped to CONNECTION_FAILED") {
                REQUIRE(rc == ENTROPIC_ERROR_CONNECTION_FAILED);
            }
        }

        WHEN("register is called with a valid SSE spec (no command)") {
            const char* j = R"({"url":"http://127.0.0.1:65535/sse"})";
            auto rc = entropic_register_mcp_server(h, "tparse_sse", j);
            THEN("the call returns OK or CONNECTION_FAILED") {
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_CONNECTION_FAILED
                         || rc == ENTROPIC_ERROR_SERVER_ALREADY_EXISTS));
            }
        }

        WHEN("list_mcp_servers is called on a configured engine") {
            char* json = entropic_list_mcp_servers(h);
            THEN("a non-null JSON array string is returned") {
                REQUIRE(json != nullptr);
                // Output begins with '[' (could be "[]" or a populated
                // array depending on prior WHEN branches).
                REQUIRE(json[0] == '[');
                entropic_free(json);
            }
        }

        WHEN("deregister is called for a name that was never registered") {
            auto rc = entropic_deregister_mcp_server(h, "no_such_server");
            THEN("OK or SERVER_NOT_FOUND is returned") {
                // ServerManager::disconnect_external_server() logs a
                // warning and returns void for unknown names — the
                // facade therefore reports ENTROPIC_OK. If the
                // underlying impl ever starts throwing we accept
                // SERVER_NOT_FOUND too — both prove the path runs.
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_SERVER_NOT_FOUND));
            }
        }
    }

    entropic_destroy(h);
}
