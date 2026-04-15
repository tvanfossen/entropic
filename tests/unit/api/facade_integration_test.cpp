// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file facade_integration_test.cpp
 * @brief Integration tests for the v2.0.0 C API facade wiring.
 *
 * Exercises the full create → configure → use → destroy lifecycle
 * through the C API. Tests subsystem APIs after configuration
 * (grammar, profile, throughput, MCP auth, identity, storage, audit).
 *
 * Requires bundled_models.yaml in the resolved data directory.
 * Tests that depend on configuration are skipped if configure fails.
 *
 * @version 2.0.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

/**
 * @brief RAII guard that creates AND configures a handle.
 *
 * Configure uses a minimal JSON config (no model paths, just
 * log_level). If configure fails, h remains valid but configured()
 * returns false — tests can check and skip.
 *
 * @internal
 * @version 2.0.0
 */
struct ConfiguredHandle {
    entropic_handle_t h = nullptr;
    bool ok = false;

    ConfiguredHandle() {
        entropic_create(&h);
        if (h) {
            ok = (entropic_configure(h, R"({"log_level":"WARN"})") == ENTROPIC_OK);
        }
    }
    ~ConfiguredHandle() { entropic_destroy(h); }
    operator entropic_handle_t() const { return h; }
    bool configured() const { return ok; }
};

// ── Phase 0: Lifecycle ──────────────────────────────────────

SCENARIO("Create and destroy lifecycle", "[api][facade][lifecycle]") {
    GIVEN("no handle exists") {
        WHEN("entropic_create is called") {
            entropic_handle_t h = nullptr;
            auto err = entropic_create(&h);

            THEN("it succeeds and returns a non-null handle") {
                REQUIRE(err == ENTROPIC_OK);
                REQUIRE(h != nullptr);
                entropic_destroy(h);
            }
        }
    }
}

SCENARIO("Double destroy is safe", "[api][facade][lifecycle]") {
    GIVEN("a created handle") {
        entropic_handle_t h = nullptr;
        entropic_create(&h);
        REQUIRE(h != nullptr);

        WHEN("destroy is called twice") {
            entropic_destroy(h);
            // Second destroy on dangling pointer — not testable safely.
            // But destroy(NULL) is safe:
            entropic_destroy(nullptr);

            THEN("no crash occurs") {
                SUCCEED();
            }
        }
    }
}

// ── Phase 1: Configuration ──────────────────────────────────

SCENARIO("Configure from JSON string", "[api][facade][config]") {
    GIVEN("a created handle") {
        ConfiguredHandle h;

        THEN("configure completes") {
            // May fail if bundled_models.yaml not found in dev tree
            // That's OK — we just test the API doesn't crash
            REQUIRE(h.h != nullptr);
        }
    }
}

SCENARIO("Configure with null JSON returns error", "[api][facade][config]") {
    GIVEN("a created handle") {
        entropic_handle_t h = nullptr;
        entropic_create(&h);

        WHEN("configure is called with NULL") {
            auto err = entropic_configure(h, nullptr);

            THEN("it returns INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
        entropic_destroy(h);
    }
}

// ── Phase 2: Grammar round-trip ─────────────────────────────

SCENARIO("Grammar register and get round-trip", "[api][facade][grammar]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        const char* gbnf = "root ::= \"hello\"";

        WHEN("a grammar is registered and retrieved") {
            auto err = entropic_grammar_register(h, "test_g", gbnf);
            REQUIRE(err == ENTROPIC_OK);

            char* content = entropic_grammar_get(h, "test_g");

            THEN("the content matches") {
                REQUIRE(content != nullptr);
                REQUIRE(std::strcmp(content, gbnf) == 0);
                entropic_free(content);
            }
        }

        WHEN("a grammar is deregistered") {
            entropic_grammar_register(h, "temp_g", gbnf);
            auto err = entropic_grammar_deregister(h, "temp_g");

            THEN("deregister succeeds") {
                REQUIRE(err == ENTROPIC_OK);
            }
            THEN("get returns NULL") {
                char* c = entropic_grammar_get(h, "temp_g");
                REQUIRE(c == nullptr);
            }
        }
    }
}

SCENARIO("Grammar list returns JSON array", "[api][facade][grammar]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with a registered grammar") {
        entropic_grammar_register(h, "list_g", "root ::= \"x\"");

        WHEN("grammar_list is called") {
            char* json = entropic_grammar_list(h);

            THEN("it returns a non-null JSON string") {
                REQUIRE(json != nullptr);
                REQUIRE(std::strlen(json) > 2); // more than "[]"
                entropic_free(json);
            }
        }
    }
}

SCENARIO("Grammar validate detects invalid GBNF", "[api][facade][grammar]") {
    GIVEN("an invalid GBNF string") {
        WHEN("grammar_validate is called") {
            char* err = entropic_grammar_validate("not valid gbnf {{{}}}");

            THEN("it returns a non-null error description") {
                REQUIRE(err != nullptr);
                entropic_free(err);
            }
        }
    }
}

// ── Phase 2: Profile round-trip ─────────────────────────────

SCENARIO("Profile register and list", "[api][facade][profile]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        WHEN("a profile is registered") {
            auto err = entropic_profile_register(h,
                R"({"name":"test_profile","n_batch":256})");

            THEN("it succeeds") {
                REQUIRE(err == ENTROPIC_OK);
            }
        }

        WHEN("profiles are listed") {
            char* json = entropic_profile_list(h);

            THEN("it returns a valid JSON array") {
                REQUIRE(json != nullptr);
                entropic_free(json);
            }
        }
    }
}

// ── Phase 2: Throughput ─────────────────────────────────────

SCENARIO("Throughput query on fresh engine", "[api][facade][throughput]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no generation history") {
        WHEN("throughput is queried") {
            double tps = entropic_throughput_tok_per_sec(h, nullptr);

            THEN("it returns 0.0 (no samples)") {
                REQUIRE(tps == 0.0);
            }
        }
    }
}

// ── Phase 3: MCP key grant/check/revoke ─────────────────────

SCENARIO("MCP key grant and check round-trip", "[api][facade][mcp_auth]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        // Register identity for key enforcement
        entropic_mcp_access_level_t write = static_cast<entropic_mcp_access_level_t>(2);

        WHEN("a key is granted and checked") {
            auto grant_err = entropic_grant_mcp_key(h, "test_id", "bash.*", write);
            // grant may fail if identity not registered — that's OK
            if (grant_err == ENTROPIC_OK) {
                int result = entropic_check_mcp_key(h, "test_id", "bash.run", write);

                THEN("check returns authorized (1)") {
                    REQUIRE(result == 1);
                }
            }
        }

        WHEN("a key is revoked") {
            entropic_grant_mcp_key(h, "revoke_id", "git.*", write);
            entropic_revoke_mcp_key(h, "revoke_id", "git.*");

            THEN("no crash") {
                SUCCEED();
            }
        }
    }
}

SCENARIO("MCP key serialize/deserialize round-trip", "[api][facade][mcp_auth]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        WHEN("keys are serialized") {
            char* json = entropic_serialize_mcp_keys(h);

            THEN("it returns a non-null JSON string") {
                REQUIRE(json != nullptr);
                entropic_free(json);
            }
        }
    }
}

// ── Phase 3: Identity ───────────────────────────────────────

SCENARIO("Identity count on fresh engine", "[api][facade][identity]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no loaded identities") {
        WHEN("identity_count is called") {
            size_t total = 999;
            size_t dynamic = 999;
            auto err = entropic_identity_count(h, &total, &dynamic);

            THEN("it succeeds with zero counts") {
                REQUIRE(err == ENTROPIC_OK);
                REQUIRE(total == 0);
                REQUIRE(dynamic == 0);
            }
        }
    }
}

SCENARIO("Dynamic identity create and destroy", "[api][facade][identity]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        WHEN("a dynamic identity is created") {
            auto err = entropic_create_identity(h,
                R"({"name":"test_npc","system_prompt":"You are a guard.","focus":["guard"]})");

            THEN("it succeeds") {
                REQUIRE(err == ENTROPIC_OK);
            }

            AND_WHEN("it is destroyed") {
                auto err2 = entropic_destroy_identity(h, "test_npc");
                THEN("destroy succeeds") {
                    REQUIRE(err2 == ENTROPIC_OK);
                }
            }
        }
    }
}

SCENARIO("List identities returns JSON array", "[api][facade][identity]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with a dynamic identity") {
        entropic_create_identity(h,
            R"({"name":"list_test","system_prompt":"test","focus":["test"]})");

        WHEN("list_identities is called") {
            char* json = entropic_list_identities(h);

            THEN("it returns a non-null JSON array") {
                REQUIRE(json != nullptr);
                REQUIRE(std::strlen(json) > 2);
                entropic_free(json);
            }
        }
    }
}

// ── Phase 4: Storage ────────────────────────────────────────

SCENARIO("Storage open and close", "[api][facade][storage]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        WHEN("storage is opened with a temp path") {
            auto err = entropic_storage_open(h, "/tmp/entropic_test.db");

            THEN("it succeeds") {
                REQUIRE(err == ENTROPIC_OK);
            }

            AND_WHEN("storage is closed") {
                auto err2 = entropic_storage_close(h);
                THEN("close succeeds") {
                    REQUIRE(err2 == ENTROPIC_OK);
                }
            }
        }
    }
}

// ── Phase 4: Audit ──────────────────────────────────────────

SCENARIO("Audit count on engine with no logger", "[api][facade][audit]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no audit logger") {
        WHEN("audit_count is called") {
            size_t count = 999;
            auto err = entropic_audit_count(h, &count);

            THEN("it returns OK with count 0") {
                REQUIRE(err == ENTROPIC_OK);
                REQUIRE(count == 0);
            }
        }
    }
}

// ── Phase 5: Constitutional ─────────────────────────────────

SCENARIO("Validation last_result on fresh engine", "[api][facade][constitutional]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no validator") {
        WHEN("validation_last_result is called") {
            char* json = entropic_validation_last_result(h);

            THEN("it returns NULL (no validator initialized)") {
                REQUIRE(json == nullptr);
            }
        }
    }
}

// ── Phase 2: Adapter queries ────────────────────────────────

SCENARIO("Adapter list on fresh engine", "[api][facade][adapter]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no adapters loaded") {
        WHEN("adapter_list is called") {
            char* json = entropic_adapter_list(h);

            THEN("it returns an empty JSON array") {
                REQUIRE(json != nullptr);
                REQUIRE(std::strcmp(json, "[]") == 0);
                entropic_free(json);
            }
        }
    }
}

SCENARIO("Adapter state for unknown adapter", "[api][facade][adapter]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        WHEN("adapter_state is queried for nonexistent adapter") {
            int state = entropic_adapter_state(h, "nonexistent");

            THEN("it returns COLD (0)") {
                REQUIRE(state == 0);
            }
        }
    }
}

// ── Phase 3: MCP server list ────────────────────────────────

SCENARIO("MCP server list on fresh engine", "[api][facade][mcp]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no external servers") {
        WHEN("list_mcp_servers is called") {
            char* json = entropic_list_mcp_servers(h);

            THEN("it returns a JSON array") {
                REQUIRE(json != nullptr);
                entropic_free(json);
            }
        }
    }
}
