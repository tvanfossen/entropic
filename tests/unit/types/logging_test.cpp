// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_logging.cpp
 * @brief BDD tests for spdlog initialization pattern.
 * @version 1.8.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/logging.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

SCENARIO("Logging initialization is idempotent", "[logging][types]") {
    GIVEN("The logging subsystem") {
        WHEN("init is called multiple times") {
            entropic::log::init(spdlog::level::info);
            entropic::log::init(spdlog::level::debug);
            entropic::log::init(spdlog::level::warn);

            THEN("no crash occurs and system remains functional") {
                auto logger = entropic::log::get("test-idempotent");
                REQUIRE(logger != nullptr);
            }
        }
    }
}

SCENARIO("Named loggers are created on demand", "[logging][types]") {
    GIVEN("The logging subsystem is initialized") {
        entropic::log::init(spdlog::level::info);

        WHEN("a named logger is requested") {
            auto logger = entropic::log::get("test-named");

            THEN("it returns a non-null logger") {
                REQUIRE(logger != nullptr);
            }

            THEN("the logger has the requested name") {
                REQUIRE(logger->name() == "test-named");
            }
        }
    }
}

SCENARIO("Repeated get() returns the same logger", "[logging][types]") {
    GIVEN("A named logger has been created") {
        entropic::log::init(spdlog::level::info);
        auto first = entropic::log::get("test-same");

        WHEN("the same name is requested again") {
            auto second = entropic::log::get("test-same");

            THEN("it returns the same logger instance") {
                REQUIRE(first.get() == second.get());
            }
        }
    }
}

// ── gh#59 (v2.3.1): per-handle log isolation ─────────────────

static std::string read_file_contents(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) { return ""; }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

SCENARIO("Per-handle log scope routes session.log writes per handle "
         "(gh#59)",
         "[logging][types][gh59]") {
    GIVEN("two registered handle log dirs and a subsystem logger") {
        entropic::log::init(spdlog::level::info);

        auto base = std::filesystem::temp_directory_path() / "gh59-iso";
        std::filesystem::remove_all(base);
        auto dir_a = base / "h1";
        auto dir_b = base / "h2";
        std::filesystem::create_directories(dir_a);
        std::filesystem::create_directories(dir_b);

        // Distinct ids per handle. Real engine uses monotonic ints
        // from entropic_create; we just pick anything non-zero.
        constexpr int ID_A = 1001;
        constexpr int ID_B = 1002;
        entropic::log::register_handle_log(ID_A, dir_a);
        entropic::log::register_handle_log(ID_B, dir_b);

        // Use a previously-unseen logger name so the test isn't
        // polluted by setup-time messages from real subsystems.
        auto logger = entropic::log::get("gh59.iso.probe");

        WHEN("two scope-bracketed lines are emitted on each handle") {
            {
                entropic::log::HandleLogScope scope(ID_A);
                logger->info("ALPHA-ONLY-marker");
            }
            {
                entropic::log::HandleLogScope scope(ID_B);
                logger->info("BETA-ONLY-marker");
            }
            logger->flush();
            // Flush spdlog's registry too — defensive in case the
            // dispatcher's flush isn't reached.
            spdlog::default_logger()->flush();

            auto a = read_file_contents(dir_a / "session.log");
            auto b = read_file_contents(dir_b / "session.log");

            THEN("each marker appears only in the matching file") {
                REQUIRE(a.find("ALPHA-ONLY-marker") != std::string::npos);
                REQUIRE(a.find("BETA-ONLY-marker") == std::string::npos);

                REQUIRE(b.find("BETA-ONLY-marker") != std::string::npos);
                REQUIRE(b.find("ALPHA-ONLY-marker") == std::string::npos);
            }
        }

        entropic::log::unregister_handle_log(ID_A);
        entropic::log::unregister_handle_log(ID_B);
    }
}

SCENARIO("HandleLogScope nests correctly (gh#59)",
         "[logging][types][gh59]") {
    GIVEN("the current handle id is initially zero") {
        // current_handle_id should reset between tests because the
        // thread-local resets each test invocation in the same
        // thread. If a prior scenario leaked an outer scope this
        // would catch it.
        REQUIRE(entropic::log::current_handle_id() == 0);

        WHEN("scopes are nested") {
            {
                entropic::log::HandleLogScope outer(7);
                REQUIRE(entropic::log::current_handle_id() == 7);
                {
                    entropic::log::HandleLogScope inner(13);
                    REQUIRE(entropic::log::current_handle_id() == 13);
                }
                THEN("inner exit restores outer") {
                    REQUIRE(entropic::log::current_handle_id() == 7);
                }
            }

            THEN("outer exit restores zero") {
                REQUIRE(entropic::log::current_handle_id() == 0);
            }
        }
    }
}

SCENARIO("setup_session + register_handle_log don't double-write "
         "(gh#67 v2.3.1 regression)",
         "[logging][types][gh67]") {
    // gh#67: v2.3.1's setup_session() still added a file sink to the
    // global spdlog tree even though register_handle_log() (also
    // called from configure_dir) attached the same file via the
    // dispatcher. Every log line ended up written twice. This test
    // would have caught it pre-ship.
    GIVEN("a handle log dir and BOTH setup_session + register_handle_log "
          "called against it") {
        entropic::log::init(spdlog::level::info);

        auto base = std::filesystem::temp_directory_path() / "gh67-dup";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);
        constexpr int ID = 3001;

        // Mirror what entropic_configure_dir does: call BOTH.
        entropic::log::setup_session(base);
        entropic::log::register_handle_log(ID, base);

        WHEN("a single line is emitted under the handle's scope") {
            auto logger = entropic::log::get("gh67.dup.probe");
            {
                entropic::log::HandleLogScope scope(ID);
                logger->info("UNIQUE-DUP-CHECK-marker");
            }
            spdlog::default_logger()->flush();
            logger->flush();

            std::ifstream in(base / "session.log");
            std::ostringstream ss;
            ss << in.rdbuf();
            auto contents = ss.str();

            THEN("the marker appears exactly once in session.log") {
                size_t pos = contents.find("UNIQUE-DUP-CHECK-marker");
                REQUIRE(pos != std::string::npos);
                size_t second = contents.find(
                    "UNIQUE-DUP-CHECK-marker", pos + 1);
                INFO("file contents:\n" << contents);
                REQUIRE(second == std::string::npos);
            }
        }

        entropic::log::unregister_handle_log(ID);
    }
}

SCENARIO("Lines emitted with no handle scope route nowhere by handle "
         "(gh#59)",
         "[logging][types][gh59]") {
    GIVEN("a registered handle and a logger emitting outside any scope") {
        entropic::log::init(spdlog::level::info);

        auto base = std::filesystem::temp_directory_path() / "gh59-no-scope";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);
        constexpr int ID = 2001;
        entropic::log::register_handle_log(ID, base);

        WHEN("a line is emitted with no HandleLogScope active") {
            REQUIRE(entropic::log::current_handle_id() == 0);
            auto logger = entropic::log::get("gh59.noscope.probe");
            logger->info("ORPHAN-marker");
            spdlog::default_logger()->flush();

            auto contents = read_file_contents(base / "session.log");

            THEN("it does NOT appear in the registered handle's file") {
                // Stderr still gets it (process-global console). The
                // handle's session.log is the assertion target — no
                // bleed into the wrong file.
                REQUIRE(contents.find("ORPHAN-marker") == std::string::npos);
            }
        }

        entropic::log::unregister_handle_log(ID);
    }
}
