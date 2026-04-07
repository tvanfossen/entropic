/**
 * @file test_audit_log.cpp
 * @brief BDD subsystem test — audit entries recorded on tool execution.
 *
 * Validates that AuditLogger records a tool call entry to JSONL and
 * the entry can be read back with correct field values.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

#include <entropic/storage/audit_logger.h>

#include <fstream>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test: Audit Log ─────────────────────────────────────────

SCENARIO("Audit entry is recorded when a tool call executes",
         "[model][audit_log]")
{
    GIVEN("a model loaded and AuditLogger configured") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test_audit_log");

        auto tmp_dir = fs::temp_directory_path()
            / "entropic_test_audit";
        fs::create_directories(tmp_dir);

        AuditLogConfig config;
        config.log_dir = tmp_dir;
        config.session_id = "test-session-001";
        config.enabled = true;
        config.flush_interval_entries = 0;

        AuditLogger logger(config);
        REQUIRE(logger.initialize());

        WHEN("an audit entry is recorded and flushed") {
            AuditEntry entry;
            entry.caller_id = "lead";
            entry.tool_name = "filesystem.read_file";
            entry.params_json = R"({"path":"test.txt"})";
            entry.result_status = "success";
            entry.result_content = "hello world";
            entry.elapsed_ms = 5.0;
            entry.directives_json = "[]";

            logger.record(entry);
            logger.flush();

            THEN("the JSONL file contains the entry") {
                CHECK(logger.entry_count() == 1);

                std::ifstream fin(logger.log_path());
                std::string line;
                REQUIRE(std::getline(fin, line));
                auto j = json::parse(line);
                CHECK(j["tool_name"] == "filesystem.read_file");
                CHECK(j["result"]["status"] == "success");

                fs::remove_all(tmp_dir);
                end_test_log();
            }
        }
    }
}
