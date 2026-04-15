// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_audit_logger.cpp
 * @brief BDD tests for AuditLogger JSONL recording and rotation.
 * @version 1.9.5
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/storage/audit_logger.h>
#include <entropic/storage/audit_entry.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

/**
 * @brief RAII temp directory for audit logger tests.
 * @internal
 * @version 1.9.5
 */
struct TempAuditDir {
    fs::path dir;

    /**
     * @brief Construct with unique temp directory.
     * @version 1.9.5
     */
    TempAuditDir() {
        dir = fs::temp_directory_path() / "entropic_test" /
              ("audit_" + std::to_string(
                  reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
    }

    /**
     * @brief Destructor — remove directory.
     * @version 1.9.5
     */
    ~TempAuditDir() { fs::remove_all(dir); }
};

/**
 * @brief Create a minimal test audit entry.
 * @return AuditEntry with basic fields set.
 * @internal
 * @version 1.9.5
 */
static entropic::AuditEntry make_entry() {
    entropic::AuditEntry e;
    e.caller_id = "lead";
    e.tool_name = "bash.execute";
    e.params_json = R"({"command":"ls"})";
    e.result_status = "success";
    e.result_content = "file1.txt\nfile2.txt";
    e.elapsed_ms = 12.5;
    e.directives_json = "[]";
    return e;
}

/**
 * @brief Read all lines from a file.
 * @param path File path.
 * @return Vector of lines.
 * @internal
 * @version 1.9.5
 */
static std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

SCENARIO("AuditLogger creates file on initialize",
         "[storage][audit]") {
    GIVEN("An AuditLogger with a temp directory") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-session-1";
        entropic::AuditLogger logger(cfg);

        WHEN("initialize is called") {
            bool ok = logger.initialize();

            THEN("audit.jsonl exists") {
                REQUIRE(ok);
                REQUIRE(fs::exists(tmp.dir / "audit.jsonl"));
            }
        }
    }
}

SCENARIO("AuditLogger records a single JSONL line",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-session-2";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("one entry is recorded") {
            logger.record(make_entry());

            THEN("file has one parseable JSON line") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 1);
                auto j = nlohmann::json::parse(lines[0]);
                REQUIRE(j["version"] == 1);
                REQUIRE(j["session_id"] == "test-session-2");
                REQUIRE(j["tool_name"] == "bash.execute");
            }
        }
    }
}

SCENARIO("AuditLogger records multiple entries",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-multi";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("10 entries are recorded") {
            for (int i = 0; i < 10; ++i) {
                logger.record(make_entry());
            }

            THEN("file has 10 lines") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 10);
            }
        }
    }
}

SCENARIO("AuditLogger sequence numbers are monotonic",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-seq";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("5 entries are recorded") {
            for (int i = 0; i < 5; ++i) {
                logger.record(make_entry());
            }

            THEN("sequence numbers are 0, 1, 2, 3, 4") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                for (int i = 0; i < 5; ++i) {
                    auto j = nlohmann::json::parse(lines[i]);
                    REQUIRE(j["sequence"] == i);
                }
            }
        }
    }
}

SCENARIO("AuditLogger concurrent writes produce no duplicates",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-concurrent";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("4 threads each record 25 entries") {
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&logger]() {
                    for (int i = 0; i < 25; ++i) {
                        logger.record(make_entry());
                    }
                });
            }
            for (auto& t : threads) {
                t.join();
            }

            THEN("100 entries with unique sequence numbers") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 100);
                std::set<int> seqs;
                for (const auto& line : lines) {
                    auto j = nlohmann::json::parse(line);
                    seqs.insert(j["sequence"].get<int>());
                }
                REQUIRE(seqs.size() == 100);
            }
        }
    }
}

SCENARIO("AuditLogger entry_count matches record calls",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-count";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("7 entries are recorded") {
            for (int i = 0; i < 7; ++i) {
                logger.record(make_entry());
            }

            THEN("entry_count returns 7") {
                REQUIRE(logger.entry_count() == 7);
            }
        }
    }
}

SCENARIO("AuditLogger flush forces write to disk",
         "[storage][audit]") {
    GIVEN("A logger with flush_interval=100 (high)") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-flush";
        cfg.flush_interval_entries = 100;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("3 entries recorded then flush called") {
            for (int i = 0; i < 3; ++i) {
                logger.record(make_entry());
            }
            logger.flush();

            THEN("all entries are readable from disk") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 3);
            }
        }
    }
}

SCENARIO("AuditLogger flush_interval batches writes",
         "[storage][audit]") {
    GIVEN("A logger with flush_interval=5") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-batch";
        cfg.flush_interval_entries = 5;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("5 entries recorded (triggers flush)") {
            for (int i = 0; i < 5; ++i) {
                logger.record(make_entry());
            }

            THEN("all 5 entries readable from disk") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 5);
            }
        }
    }
}

SCENARIO("AuditLogger appends to existing file",
         "[storage][audit]") {
    GIVEN("An existing audit.jsonl with 3 entries") {
        TempAuditDir tmp;
        {
            entropic::AuditLogConfig cfg;
            cfg.log_dir = tmp.dir;
            cfg.session_id = "session-A";
            cfg.flush_interval_entries = 0;
            entropic::AuditLogger first(cfg);
            first.initialize();
            for (int i = 0; i < 3; ++i) {
                first.record(make_entry());
            }
        }

        WHEN("a second logger appends 3 more entries") {
            entropic::AuditLogConfig cfg;
            cfg.log_dir = tmp.dir;
            cfg.session_id = "session-B";
            cfg.flush_interval_entries = 0;
            entropic::AuditLogger second(cfg);
            second.initialize();
            for (int i = 0; i < 3; ++i) {
                second.record(make_entry());
            }

            THEN("file has 6 entries total") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 6);
            }
        }
    }
}

SCENARIO("AuditLogger rotates at max_file_size",
         "[storage][audit]") {
    GIVEN("A logger with 1KB rotation limit") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-rotate";
        cfg.flush_interval_entries = 0;
        cfg.max_file_size = 1024;
        cfg.max_files = 3;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("enough entries to exceed 1KB") {
            auto entry = make_entry();
            entry.result_content = std::string(200, 'X');
            for (int i = 0; i < 20; ++i) {
                logger.record(entry);
            }

            THEN("rotated file exists") {
                REQUIRE(fs::exists(
                    tmp.dir / "audit.jsonl.1"));
            }
        }
    }
}

SCENARIO("AuditLogger rotation respects max_files",
         "[storage][audit]") {
    GIVEN("A logger with max_files=2 and tiny rotation") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-maxfiles";
        cfg.flush_interval_entries = 0;
        cfg.max_file_size = 512;
        cfg.max_files = 2;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("many entries force multiple rotations") {
            auto entry = make_entry();
            entry.result_content = std::string(200, 'Z');
            for (int i = 0; i < 50; ++i) {
                logger.record(entry);
            }

            THEN("audit.jsonl.3 does not exist (max_files=2)") {
                REQUIRE_FALSE(fs::exists(
                    tmp.dir / "audit.jsonl.3"));
            }
        }
    }
}

SCENARIO("AuditLogger log_path returns correct path",
         "[storage][audit]") {
    GIVEN("A logger with a specific directory") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-path";
        entropic::AuditLogger logger(cfg);

        WHEN("log_path is called") {
            auto path = logger.log_path();

            THEN("it matches the expected location") {
                REQUIRE(path == tmp.dir / "audit.jsonl");
            }
        }
    }
}

SCENARIO("Disabled AuditLogger creates no file",
         "[storage][audit]") {
    GIVEN("A logger with enabled=false") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-disabled";
        cfg.enabled = false;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("record is called") {
            logger.record(make_entry());

            THEN("no file is created") {
                REQUIRE_FALSE(fs::exists(
                    tmp.dir / "audit.jsonl"));
            }

            AND_THEN("entry_count is 0") {
                REQUIRE(logger.entry_count() == 0);
            }
        }
    }
}

SCENARIO("Concurrent writes produce complete JSON lines",
         "[storage][audit]") {
    GIVEN("An initialized AuditLogger") {
        TempAuditDir tmp;
        entropic::AuditLogConfig cfg;
        cfg.log_dir = tmp.dir;
        cfg.session_id = "test-no-interleave";
        cfg.flush_interval_entries = 0;
        entropic::AuditLogger logger(cfg);
        logger.initialize();

        WHEN("4 threads each record 25 entries") {
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&logger]() {
                    for (int i = 0; i < 25; ++i) {
                        logger.record(make_entry());
                    }
                });
            }
            for (auto& t : threads) {
                t.join();
            }

            THEN("every line is valid JSON") {
                auto lines = read_lines(tmp.dir / "audit.jsonl");
                REQUIRE(lines.size() == 100);
                for (const auto& line : lines) {
                    REQUIRE_NOTHROW(nlohmann::json::parse(line));
                }
            }
        }
    }
}
