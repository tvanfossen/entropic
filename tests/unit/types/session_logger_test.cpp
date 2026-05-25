// SPDX-License-Identifier: Apache-2.0
/**
 * @file session_logger_test.cpp
 * @brief BDD tests for SessionLogger (session_model.log writer).
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/session_logger.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

/**
 * @brief RAII tmp dir for a single test scenario.
 *
 * Catch2 doesn't ship a tmp-dir helper, so each scenario that writes
 * session_model.log uses one of these to isolate its filesystem
 * side-effects from other scenarios.
 *
 * @utility
 * @version 2.3.10
 */
struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        char tmpl[] = "/tmp/entropic-session-logger-XXXXXX";
        const char* dir = mkdtemp(tmpl);
        REQUIRE(dir != nullptr);
        path = dir;
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

/**
 * @brief Read full file contents into a string.
 * @param p File path.
 * @return Whole-file content (empty if file is missing).
 * @utility
 * @version 2.3.10
 */
std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) { return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace

SCENARIO("SessionLogger constructs and writes to session_model.log",
         "[session_logger][types]")
{
    TmpDir dir;

    GIVEN("a SessionLogger opened against a valid directory") {
        entropic::SessionLogger logger(dir.path);

        THEN("is_open() reports the file handle is live") {
            REQUIRE(logger.is_open());
        }

        WHEN("log_user_input is called") {
            logger.log_user_input("hello there");

            // SessionLogger flushes on each write; we can read while
            // it's still in scope.
            auto contents = slurp(dir.path / "session_model.log");
            THEN("the user delimiter, payload, and assistant delimiter land") {
                REQUIRE(contents.find("--- USER ---") != std::string::npos);
                REQUIRE(contents.find("hello there") != std::string::npos);
                REQUIRE(contents.find("--- ASSISTANT ---") != std::string::npos);
            }
        }

        WHEN("log_raw_token is called with a normal token") {
            logger.log_user_input("prompt");
            logger.log_raw_token("a", 1);
            logger.log_raw_token("bc", 2);
            logger.log_raw_token("def", 3);

            auto contents = slurp(dir.path / "session_model.log");
            THEN("tokens land in order after the assistant delimiter") {
                auto pos = contents.find("--- ASSISTANT ---");
                REQUIRE(pos != std::string::npos);
                auto after = contents.substr(pos);
                REQUIRE(after.find("abcdef") != std::string::npos);
            }
        }

        WHEN("end_turn is called after assistant output") {
            logger.log_user_input("hi");
            logger.log_raw_token("ok", 2);
            logger.end_turn();

            auto contents = slurp(dir.path / "session_model.log");
            THEN("trailing newlines separate turns") {
                // end_turn appends "\n\n" after the assistant tokens.
                REQUIRE(contents.size() >= 4);
                REQUIRE(contents.substr(contents.size() - 2) == "\n\n");
            }
        }
    }
}

SCENARIO("SessionLogger refuses raw_token writes with degenerate inputs",
         "[session_logger][types][failure-mode]")
{
    TmpDir dir;
    entropic::SessionLogger logger(dir.path);
    REQUIRE(logger.is_open());
    logger.log_user_input("prompt");

    // Capture pre-call size so we can prove the degenerate calls don't write.
    auto before = slurp(dir.path / "session_model.log").size();

    WHEN("log_raw_token is called with nullptr token pointer") {
        logger.log_raw_token(nullptr, 5);
        THEN("nothing is written to the log") {
            REQUIRE(slurp(dir.path / "session_model.log").size() == before);
        }
    }

    WHEN("log_raw_token is called with zero length") {
        logger.log_raw_token("ignored", 0);
        THEN("nothing is written to the log") {
            REQUIRE(slurp(dir.path / "session_model.log").size() == before);
        }
    }
}

SCENARIO("SessionLogger handles an un-openable log path gracefully",
         "[session_logger][types][failure-mode]")
{
    GIVEN("a SessionLogger opened against a path that cannot be created") {
        // /proc is read-only — fopen will fail under any subdir.
        std::filesystem::path bad_dir = "/proc/sys/kernel/bad-entropic-tmp";
        entropic::SessionLogger logger(bad_dir);

        THEN("is_open() reports false (no crash, ctor returned)") {
            REQUIRE_FALSE(logger.is_open());
        }

        WHEN("log_user_input is called on an un-openable logger") {
            logger.log_user_input("anything");
            THEN("the call returns cleanly (no crash, no throw)") {
                REQUIRE_FALSE(logger.is_open());
            }
        }

        WHEN("log_raw_token is called on an un-openable logger") {
            logger.log_raw_token("x", 1);
            THEN("the call returns cleanly") {
                REQUIRE_FALSE(logger.is_open());
            }
        }

        WHEN("end_turn is called on an un-openable logger") {
            logger.end_turn();
            THEN("the call returns cleanly") {
                REQUIRE_FALSE(logger.is_open());
            }
        }
    }
}

SCENARIO("SessionLogger raw_token_callback dispatches to the instance",
         "[session_logger][types][callback]")
{
    TmpDir dir;
    entropic::SessionLogger logger(dir.path);
    logger.log_user_input("via callback");

    WHEN("raw_token_callback is invoked with the logger as user_data") {
        const char* payload = "streamed";
        entropic::SessionLogger::raw_token_callback(
            payload, 8, &logger);

        auto contents = slurp(dir.path / "session_model.log");
        THEN("the callback wrote the token through log_raw_token") {
            REQUIRE(contents.find("streamed") != std::string::npos);
        }
    }
}

SCENARIO("SessionLogger destructor closes the log on scope exit",
         "[session_logger][types][raii]")
{
    TmpDir dir;

    WHEN("a SessionLogger is constructed and destroyed in a nested scope") {
        {
            entropic::SessionLogger logger(dir.path);
            logger.log_user_input("destruct test");
            REQUIRE(logger.is_open());
            // ~SessionLogger() fires here on scope exit.
        }

        THEN("the file is closed and content is fully flushed") {
            // We can't directly observe FILE* closure, but the
            // file should be on disk with the written content
            // (fflush ran on each call; close also flushes).
            auto contents = slurp(dir.path / "session_model.log");
            REQUIRE(contents.find("destruct test") != std::string::npos);
        }
    }
}
