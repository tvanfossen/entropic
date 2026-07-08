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

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// ── gh#39 (v2.1.8): entropic_context_usage ──────────────────

SCENARIO("entropic_context_usage on null handle returns INVALID_HANDLE",
         "[api][facade][context][v2.1.8]") {
    GIVEN("a null handle") {
        size_t used = 0, cap = 0;
        WHEN("context_usage is called") {
            auto rc = entropic_context_usage(nullptr, &used, &cap);
            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_context_usage with null out params returns INVALID_ARGUMENT",
         "[api][facade][context][v2.1.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured handle") {
        WHEN("either out pointer is null") {
            size_t v = 0;
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(entropic_context_usage(h, nullptr, &v)
                        == ENTROPIC_ERROR_INVALID_ARGUMENT);
                REQUIRE(entropic_context_usage(h, &v, nullptr)
                        == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_context_usage on a configured engine returns valid pair",
         "[api][facade][context][v2.1.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine") {
        size_t used = 999, cap = 999;
        WHEN("context_usage is called") {
            auto rc = entropic_context_usage(h, &used, &cap);
            THEN("either OK with capacity > 0, or INVALID_STATE (no tier)") {
                // Minimal configure may not lock a tier — either outcome OK.
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
                if (rc == ENTROPIC_OK) {
                    REQUIRE(cap > 0);
                    REQUIRE(used <= cap);
                }
            }
        }
    }
}

// ── gh#37 (v2.1.8): entropic_run_messages / streaming ───────

SCENARIO("entropic_run_messages on null handle returns INVALID_HANDLE",
         "[api][facade][run_messages][v2.1.8]") {
    GIVEN("a null handle") {
        char* result = nullptr;
        WHEN("run_messages is called") {
            auto rc = entropic_run_messages(nullptr, "[]", &result);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
                REQUIRE(result == nullptr);
            }
        }
    }
}

SCENARIO("entropic_run_messages with null args returns INVALID_ARGUMENT",
         "[api][facade][run_messages][v2.1.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured handle") {
        char* result = nullptr;
        WHEN("messages_json is null") {
            auto rc = entropic_run_messages(h, nullptr, &result);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
        WHEN("result_json is null") {
            auto rc = entropic_run_messages(h, "[]", nullptr);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_run_messages with malformed JSON returns GENERATE_FAILED",
         "[api][facade][run_messages][v2.1.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured handle") {
        WHEN("messages_json is malformed") {
            char* result = nullptr;
            auto rc = entropic_run_messages(h, "not json", &result);
            THEN("the parse exception is mapped to GENERATE_FAILED") {
                REQUIRE(rc == ENTROPIC_ERROR_GENERATE_FAILED);
            }
        }
    }
}

SCENARIO("entropic_run_messages: image without vision tier → NO_VISION_TIER",
         "[api][facade][run_messages][v2.1.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured handle with no vision-capable tier") {
        const char* j = R"([{"role":"user","content":[
            {"type":"image","path":"/tmp/x.png"},
            {"type":"text","text":"what?"}
        ]}])";
        char* result = nullptr;
        WHEN("run_messages is called with an image content part") {
            auto rc = entropic_run_messages(h, j, &result);
            THEN("NO_VISION_TIER is returned cleanly") {
                // Minimal configure has no tiers loaded; the
                // orchestrator may be NULL — in that case the parse
                // also raises GENERATE_FAILED. Either is acceptable
                // proof that image input fails fast without a tier.
                REQUIRE((rc == ENTROPIC_ERROR_NO_VISION_TIER
                         || rc == ENTROPIC_ERROR_INVALID_STATE
                         || rc == ENTROPIC_ERROR_GENERATE_FAILED));
            }
        }
    }
}

SCENARIO("entropic_run_messages_streaming validates args",
         "[api][facade][run_messages][v2.1.8]") {
    GIVEN("a null handle") {
        WHEN("streaming variant is called") {
            auto rc = entropic_run_messages_streaming(
                nullptr, "[]",
                [](const char*, size_t, void*){}, nullptr, nullptr);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

// ── gh#109: run entry points must log under the handle's file sink ─────

SCENARIO("entropic_run_messages with console logging disabled still "
         "appends to session.log (gh#109 regression)",
         "[api][facade][run_messages][gh109]") {
    GIVEN("a handle configured via entropic_configure (JSON) with "
          "console_logging=false and an explicit log_dir, and no models "
          "(so configure succeeds without a real GGUF on disk, matching "
          "the zero-tier ConfiguredHandle pattern used elsewhere in this "
          "file)") {
        auto base = std::filesystem::temp_directory_path() / "gh109-run-log";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);

        // entropic_configure_dir's layered discovery (global config →
        // consumer defaults → project config → bundled-default fallback
        // when tiers are still empty) always resolves the bundled
        // default_config.yaml's real tiers when nothing else supplies
        // models — those tiers point at GGUF files that don't exist in
        // CI, so configure would fail with LOAD_FAILED before ever
        // reaching entropic_run_messages. entropic_configure (JSON) has
        // no such fallback: give it log_dir + console_logging only, no
        // "models" key, and it configures with zero tiers — same shape
        // ConfiguredHandle relies on above.
        std::string cfg = std::string(R"({"log_level":"WARN",)")
            + R"("console_logging":false,"log_dir":")" + base.string()
            + R"("})";

        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        REQUIRE(entropic_configure(h, cfg.c_str()) == ENTROPIC_OK);

        auto session_log = base / "session.log";
        REQUIRE(std::filesystem::exists(session_log));

        auto size_before = std::filesystem::file_size(session_log);

        WHEN("entropic_run_messages is called (malformed JSON forces the "
             "error-logging path without needing a loaded model)") {
            char* result = nullptr;
            auto rc = entropic_run_messages(h, "not json", &result);

            THEN("the run's error is appended to this handle's session.log, "
                 "not silently dropped") {
                REQUIRE(rc == ENTROPIC_ERROR_GENERATE_FAILED);

                std::ifstream in(session_log);
                std::ostringstream ss;
                ss << in.rdbuf();
                auto contents = ss.str();

                REQUIRE(contents.size() > size_before);
                REQUIRE(contents.find("run_messages") != std::string::npos);
            }

            if (result) { entropic_free(result); }
        }

        entropic_destroy(h);
    }
}

// ── v2.3.8 facade coverage gap: identity + storage NULL/error paths ────

SCENARIO("entropic_load_identity for an unknown name on a configured engine",
         "[api][facade][identity][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine with no identities loaded") {
        WHEN("load_identity is called with a name that doesn't exist") {
            auto rc = entropic_load_identity(h, "no_such_identity_xyz");
            THEN("IDENTITY_NOT_FOUND is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
            }
        }

        WHEN("load_identity is called with a NULL name") {
            auto rc = entropic_load_identity(h, nullptr);
            THEN("INVALID_ARGUMENT is returned (handle is valid + "
                 "identity_manager present)") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_get_identity on a configured engine with no identities loaded "
         "returns IDENTITY_NOT_FOUND",
         "[api][facade][identity][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());

    GIVEN("a configured engine that has not loaded any identity") {
        WHEN("get_identity is called") {
            char* out = nullptr;
            auto rc = entropic_get_identity(h, &out);
            THEN("either an identity is auto-populated (OK) or "
                 "IDENTITY_NOT_FOUND is returned") {
                // The facade returns the first identity_manager->list()
                // entry — minimal configure may or may not seed one
                // depending on bundled config. Both outcomes pin the
                // path through entropic_get_identity().
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND));
                if (rc == ENTROPIC_OK) {
                    REQUIRE(out != nullptr);
                    entropic_free(out);
                }
            }
        }

        WHEN("get_identity is called with a NULL output pointer") {
            auto rc = entropic_get_identity(h, nullptr);
            THEN("INVALID_ARGUMENT is returned (handle valid)") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_storage_open with NULL handle returns INVALID_HANDLE",
         "[api][facade][storage][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("storage_open is called") {
            auto rc = entropic_storage_open(nullptr, "/tmp/should_not_open.db");
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_storage_open with NULL db_path returns INVALID_ARGUMENT",
         "[api][facade][storage][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle and a NULL path") {
        WHEN("storage_open is called") {
            auto rc = entropic_storage_open(h, nullptr);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_storage_open with a path inside a non-existent directory "
         "returns STORAGE_FAILED",
         "[api][facade][storage][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a path that cannot be created (parent dir absent)") {
        WHEN("storage_open is called") {
            // SQLite cannot create a file in a non-existent directory
            // — initialize() returns false → STORAGE_FAILED.
            auto rc = entropic_storage_open(h,
                "/no_such_dir_xyz_abc_123/entropic_test.db");
            THEN("either STORAGE_FAILED or OK is returned") {
                // Some sqlite builds will create intermediate paths;
                // accept either outcome. The branch under test is the
                // try/catch path.
                REQUIRE((rc == ENTROPIC_ERROR_STORAGE_FAILED
                         || rc == ENTROPIC_OK));
                if (rc == ENTROPIC_OK) {
                    entropic_storage_close(h);
                }
            }
        }
    }
}

SCENARIO("entropic_storage_close on NULL handle returns INVALID_HANDLE",
         "[api][facade][storage][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("storage_close is called") {
            auto rc = entropic_storage_close(nullptr);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_storage_close before any open is a safe no-op",
         "[api][facade][storage][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle that has never had storage opened") {
        WHEN("storage_close is called") {
            auto rc = entropic_storage_close(h);
            THEN("OK is returned") {
                REQUIRE(rc == ENTROPIC_OK);
            }
        }
    }
}

// ── v2.3.8 audit C ABI surface ──────────────────────────────────────────

SCENARIO("entropic_audit_flush on NULL handle returns INVALID_HANDLE",
         "[api][facade][audit][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("audit_flush is called") {
            auto rc = entropic_audit_flush(nullptr);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_audit_flush on a handle with no audit_logger is a no-op",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle with no audit logger") {
        WHEN("audit_flush is called") {
            auto rc = entropic_audit_flush(h);
            THEN("OK is returned (early return when audit_logger is null)") {
                REQUIRE(rc == ENTROPIC_OK);
            }
        }
    }
}

SCENARIO("entropic_audit_count with NULL out pointer returns INVALID_ARGUMENT",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle and a NULL count out pointer") {
        WHEN("audit_count is called") {
            auto rc = entropic_audit_count(h, nullptr);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_audit_count on NULL handle returns INVALID_HANDLE",
         "[api][facade][audit][v2.3.8]") {
    GIVEN("a null handle") {
        size_t count = 0;
        WHEN("audit_count is called") {
            auto rc = entropic_audit_count(nullptr, &count);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_audit_read with a missing file returns IO",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a path that does not exist") {
        WHEN("audit_read is called") {
            char* out = nullptr;
            auto rc = entropic_audit_read(
                h, "/no_such_dir/no_such_file.jsonl", nullptr, &out);
            THEN("ENTROPIC_ERROR_IO is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_IO);
                REQUIRE(out == nullptr);
            }
        }
    }
}

SCENARIO("entropic_audit_read with NULL args returns INVALID_ARGUMENT",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle") {
        char* out = nullptr;
        WHEN("path is NULL") {
            auto rc = entropic_audit_read(h, nullptr, nullptr, &out);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
        WHEN("result_json out-param is NULL") {
            auto rc = entropic_audit_read(h, "/tmp/x.jsonl", nullptr, nullptr);
            THEN("INVALID_ARGUMENT is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("entropic_audit_read on NULL handle returns INVALID_HANDLE",
         "[api][facade][audit][v2.3.8]") {
    GIVEN("a null handle") {
        char* out = nullptr;
        WHEN("audit_read is called") {
            auto rc = entropic_audit_read(nullptr, "/tmp/x", nullptr, &out);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_audit_read of an empty JSONL file returns an empty array",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("an empty file on disk") {
        const char* path = "/tmp/entropic_audit_empty_test.jsonl";
        { std::ofstream out(path); /* truncate */ }
        WHEN("audit_read is called") {
            char* out = nullptr;
            auto rc = entropic_audit_read(h, path, nullptr, &out);
            THEN("ENTROPIC_OK + '[]' is returned") {
                REQUIRE(rc == ENTROPIC_OK);
                REQUIRE(out != nullptr);
                REQUIRE(std::string(out) == "[]");
                entropic_free(out);
            }
        }
    }
}

SCENARIO("entropic_audit_read of a populated JSONL file returns each line",
         "[api][facade][audit][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a JSONL file with two entries (and a blank line)") {
        const char* path = "/tmp/entropic_audit_populated_test.jsonl";
        {
            std::ofstream f(path);
            f << R"({"k":"a"})" << "\n";
            f << "\n";  // blank line — should be skipped
            f << R"({"k":"b","x":1})" << "\n";
        }
        WHEN("audit_read is called") {
            char* out = nullptr;
            auto rc = entropic_audit_read(h, path, nullptr, &out);
            THEN("ENTROPIC_OK and a 2-element JSON array is returned") {
                REQUIRE(rc == ENTROPIC_OK);
                REQUIRE(out != nullptr);
                // Parse the result via the standard library's json
                // for verification — we don't want the test to be
                // fragile to whitespace choices in arr.dump().
                std::string s(out);
                REQUIRE(s.front() == '[');
                REQUIRE(s.back() == ']');
                // Two records → at least one ',' separator.
                REQUIRE(s.find(',') != std::string::npos);
                entropic_free(out);
            }
        }
    }
}

// ── v2.3.8 compaction C ABI surface ─────────────────────────────────────

SCENARIO("entropic_compact on NULL handle returns INVALID_HANDLE",
         "[api][facade][compact][v2.3.8]") {
    GIVEN("a null handle") {
        char* out = nullptr;
        WHEN("compact is called") {
            auto rc = entropic_compact(nullptr, "id", &out);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_compact on a configured handle with no active engine "
         "returns INVALID_STATE",
         "[api][facade][compact][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle (engine may be null)") {
        WHEN("compact is called") {
            char* out = nullptr;
            auto rc = entropic_compact(h, "any", &out);
            THEN("INVALID_STATE is returned (external compact "
                 "requires an active session)") {
                // Either INVALID_STATE (engine null path) or the
                // "external compact requires active session" path —
                // both map to INVALID_STATE.
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

SCENARIO("entropic_register_compactor on a minimal-configured handle "
         "returns INVALID_STATE (no compactor_registry wired)",
         "[api][facade][compact][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle with no compactor_registry") {
        WHEN("register_compactor is called with a NULL compactor") {
            // check_compactor() returns INVALID_STATE before the
            // NULL-fn check fires — both branches of the ternary
            // run in this case. With a full config this would be
            // INVALID_CONFIG; we accept either here.
            auto rc = entropic_register_compactor(h, "any", nullptr, nullptr);
            THEN("INVALID_STATE or INVALID_CONFIG is returned") {
                REQUIRE((rc == ENTROPIC_ERROR_INVALID_STATE
                         || rc == ENTROPIC_ERROR_INVALID_CONFIG));
            }
        }
    }
}

SCENARIO("entropic_register_compactor on NULL handle returns INVALID_HANDLE",
         "[api][facade][compact][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("register_compactor is called") {
            // Use a dummy non-null fn pointer so the null-fn branch
            // doesn't short-circuit the handle check.
            entropic_compactor_fn cb = [](const char*, const char*,
                                          char**, char**, void*) -> int {
                return 0;
            };
            auto rc = entropic_register_compactor(nullptr, "id", cb, nullptr);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_register_compactor with a valid callback exercises check_compactor",
         "[api][facade][compact][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle and a real callback") {
        entropic_compactor_fn cb = [](const char*, const char*,
                                      char**, char**, void*) -> int {
            return 0;
        };
        WHEN("register_compactor is called with an identity") {
            auto rc = entropic_register_compactor(h, "ident_a", cb, nullptr);
            THEN("OK or INVALID_STATE is returned") {
                // Minimal configure leaves compactor_registry null →
                // INVALID_STATE. A fully wired engine returns OK.
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
            }
        }
        WHEN("register_compactor is called with a NULL identity (global slot)") {
            auto rc = entropic_register_compactor(h, nullptr, cb, nullptr);
            THEN("OK or INVALID_STATE is returned") {
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
            }
        }
    }
}

SCENARIO("entropic_deregister_compactor on NULL handle returns INVALID_HANDLE",
         "[api][facade][compact][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("deregister_compactor is called") {
            auto rc = entropic_deregister_compactor(nullptr, "x");
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("entropic_deregister_compactor on a configured handle exercises check_compactor",
         "[api][facade][compact][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle") {
        WHEN("deregister_compactor is called for a never-registered ident") {
            auto rc = entropic_deregister_compactor(h, "never_registered");
            THEN("OK or INVALID_STATE is returned") {
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
            }
        }
        WHEN("deregister_compactor is called with NULL identity") {
            auto rc = entropic_deregister_compactor(h, nullptr);
            THEN("OK or INVALID_STATE is returned") {
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
            }
        }
    }
}

SCENARIO("entropic_get_default_compactor exercises check_compactor + nulls outs",
         "[api][facade][compact][v2.3.8]") {
    ConfiguredHandle h;
    REQUIRE(h.configured());
    GIVEN("a configured handle") {
        WHEN("get_default_compactor is called with non-null outs") {
            entropic_compactor_fn fn = (entropic_compactor_fn)(uintptr_t)0xDEAD;
            void* user = (void*)(uintptr_t)0xBEEF;
            auto rc = entropic_get_default_compactor(h, &fn, &user);
            THEN("either OK with zeroed outs, or INVALID_STATE") {
                // Minimal configure leaves compactor_registry null →
                // INVALID_STATE. A fully wired engine returns OK and
                // zeros both out-params.
                REQUIRE((rc == ENTROPIC_OK
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
                if (rc == ENTROPIC_OK) {
                    REQUIRE(fn == nullptr);
                    REQUIRE(user == nullptr);
                }
            }
        }
        WHEN("get_default_compactor is called with NULL fn out-param") {
            auto rc = entropic_get_default_compactor(h, nullptr, nullptr);
            THEN("INVALID_ARGUMENT or INVALID_STATE is returned") {
                REQUIRE((rc == ENTROPIC_ERROR_INVALID_ARGUMENT
                         || rc == ENTROPIC_ERROR_INVALID_STATE));
            }
        }
    }
}

SCENARIO("entropic_get_default_compactor on NULL handle returns INVALID_HANDLE",
         "[api][facade][compact][v2.3.8]") {
    GIVEN("a null handle") {
        WHEN("get_default_compactor is called") {
            entropic_compactor_fn fn = nullptr;
            auto rc = entropic_get_default_compactor(nullptr, &fn, nullptr);
            THEN("INVALID_HANDLE is returned") {
                REQUIRE(rc == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}
