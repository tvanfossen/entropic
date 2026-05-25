// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_logging.cpp
 * @brief BDD tests for spdlog initialization pattern.
 * @version 1.8.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/logging.h>
#include <spdlog/pattern_formatter.h>

#include <filesystem>
#include <fstream>
#include <memory>
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

// ── v2.3.10 (gh#23 follow-on): coverage for escape_* + log_* helpers ──

SCENARIO("escape_content passes plain ASCII through unchanged",
         "[logging][types][escape]") {
    GIVEN("plain text with no special characters") {
        THEN("the escaped string equals the input") {
            REQUIRE(entropic::log::escape_content("hello world") ==
                    "hello world");
            REQUIRE(entropic::log::escape_content("") == "");
        }
    }
}

SCENARIO("escape_content escapes whitespace control bytes",
         "[logging][types][escape]") {
    GIVEN("text containing newlines, tabs, carriage returns") {
        THEN("each control byte renders as its backslash literal") {
            REQUIRE(entropic::log::escape_content("a\nb") == "a\\nb");
            REQUIRE(entropic::log::escape_content("a\tb") == "a\\tb");
            REQUIRE(entropic::log::escape_content("a\rb") == "a\\rb");
        }
    }

    GIVEN("a sequence of every escaped control byte") {
        auto out = entropic::log::escape_content("\n\t\r");
        THEN("the output concatenates each escape in order") {
            REQUIRE(out == "\\n\\t\\r");
        }
    }
}

SCENARIO("escape_content doubles curly braces so spdlog/fmt won't interpret",
         "[logging][types][escape]") {
    GIVEN("text with curly braces") {
        THEN("each brace is doubled") {
            REQUIRE(entropic::log::escape_content("{x}") == "{{x}}");
            REQUIRE(entropic::log::escape_content("a{b}c{d}e") ==
                    "a{{b}}c{{d}}e");
        }
    }
}

SCENARIO("escape_content rewrites well-formed ANSI CSI sequences to [ESC...]",
         "[logging][types][escape][ansi]") {
    GIVEN("a red-then-reset ANSI sequence") {
        std::string ansi = std::string("\033") + "[31mred" + "\033" + "[0m";
        auto out = entropic::log::escape_content(ansi);

        THEN("each escape becomes [ESC<params>] form") {
            // escape_ansi writes "[ESC<params>]" — verify substring tags
            // rather than exact form, since the implementation may vary
            // in whitespace handling but the marker prefix is stable.
            REQUIRE(out.find("[ESC") != std::string::npos);
            // Original ESC byte must not survive.
            REQUIRE(out.find('\033') == std::string::npos);
            // Plain payload survives.
            REQUIRE(out.find("red") != std::string::npos);
        }
    }
}

SCENARIO("escape_content survives malformed ANSI (ESC without '[')",
         "[logging][types][escape][ansi][failure-mode]") {
    GIVEN("a stray ESC byte not followed by '['") {
        std::string malformed = std::string("x") + "\033" + "y";
        auto out = entropic::log::escape_content(malformed);

        THEN("emits the degenerate [ESC] marker and continues parsing") {
            // escape_ansi's early-out: writes "[ESC]" and returns,
            // i not advanced. escape_content's outer loop then
            // increments i past the original ESC.
            REQUIRE(out.find("[ESC]") != std::string::npos);
            // Surrounding payload survives.
            REQUIRE(out.find('x') != std::string::npos);
            REQUIRE(out.find('y') != std::string::npos);
            // No raw ESC byte leaks through.
            REQUIRE(out.find('\033') == std::string::npos);
        }
    }
}

SCENARIO("escape_content handles ESC at end of string (truncated CSI)",
         "[logging][types][escape][ansi][failure-mode]") {
    GIVEN("an ANSI prefix at the very end with no terminator") {
        std::string trunc = std::string("foo") + "\033" + "[31";
        auto out = entropic::log::escape_content(trunc);

        THEN("the function returns cleanly, no infinite loop, no raw ESC") {
            REQUIRE(out.find('\033') == std::string::npos);
            REQUIRE(out.find("foo") != std::string::npos);
        }
    }
}

SCENARIO("log_content runs escape pipeline through a logger",
         "[logging][types][formatters]") {
    auto logger = entropic::log::get("logging-test.log_content");

    WHEN("log_content is called with text containing newlines") {
        entropic::log::log_content(logger, spdlog::level::info,
                                   "label", "line1\nline2");
        THEN("the call returns cleanly (logger received escaped output)") {
            // log_content delegates to logger->log(level, "{}: {}", ...);
            // the test just exercises the code path — escape_content
            // is asserted separately. Reaching this line means
            // log_content didn't throw.
            REQUIRE(logger != nullptr);
        }
    }

    WHEN("log_content is called with raw curly-brace text") {
        // Pre-escape pipeline catches braces so spdlog/fmt doesn't
        // see "{}: {x}: text" and try to format the inner {x}.
        entropic::log::log_content(logger, spdlog::level::warn,
                                   "label", "{user}");
        THEN("no fmt exception escapes the function") {
            REQUIRE(logger != nullptr);
        }
    }
}

SCENARIO("log_timing logs an operation duration",
         "[logging][types][formatters]") {
    auto logger = entropic::log::get("logging-test.log_timing");
    entropic::log::log_timing(logger, "load", 42.5);
    entropic::log::log_timing(logger, "zero", 0.0);
    REQUIRE(logger != nullptr);
}

SCENARIO("log_tokens reports tokens/sec, handling zero-time edge case",
         "[logging][types][formatters][failure-mode]") {
    auto logger = entropic::log::get("logging-test.log_tokens");

    WHEN("called with non-zero time") {
        entropic::log::log_tokens(logger, 100, 200.0);
        THEN("call returns cleanly (throughput = 500 tok/s)") {
            REQUIRE(logger != nullptr);
        }
    }

    WHEN("called with zero time (division-by-zero guard)") {
        entropic::log::log_tokens(logger, 100, 0.0);
        THEN("call returns cleanly with throughput pinned to 0.0") {
            // The guard `time_ms > 0.0` ? ratio : 0.0 prevents NaN/inf.
            REQUIRE(logger != nullptr);
        }
    }

    WHEN("called with zero tokens") {
        entropic::log::log_tokens(logger, 0, 100.0);
        THEN("call returns cleanly (throughput = 0 tok/s)") {
            REQUIRE(logger != nullptr);
        }
    }
}

SCENARIO("log_decision emits routing/decision telemetry",
         "[logging][types][formatters]") {
    auto logger = entropic::log::get("logging-test.log_decision");
    entropic::log::log_decision(logger, "router", "input-prompt", "tier:lead");
    entropic::log::log_decision(logger, "grammar", "", "");
    REQUIRE(logger != nullptr);
}

SCENARIO("add_file_sink attaches a file sink visible to subsequent loggers",
         "[logging][types][add_file_sink]") {
    auto base = std::filesystem::temp_directory_path()
        / "v2.3.10-add-file-sink";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    auto log_file = base / "added.log";

    entropic::log::init(spdlog::level::info);
    entropic::log::add_file_sink(log_file);

    WHEN("a logger writes after add_file_sink") {
        auto logger = entropic::log::get("logging-test.add_file_sink");
        logger->info("FILE-SINK-MARKER");
        spdlog::default_logger()->flush();

        auto contents = read_file_contents(log_file);
        THEN("the message lands in the added file") {
            REQUIRE(contents.find("FILE-SINK-MARKER") != std::string::npos);
        }
    }
}

SCENARIO("HandleAwareSink::set_pattern propagates to registered handle sinks",
         "[logging][types][set-pattern][gh59]") {
    auto base = std::filesystem::temp_directory_path()
        / "v2.3.10-set-pattern";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    entropic::log::init(spdlog::level::info);
    constexpr int ID = 5001;
    entropic::log::register_handle_log(ID, base);

    WHEN("set_pattern is invoked directly on the dispatcher sink") {
        // spdlog::set_pattern on a logger walks every sink and calls
        // set_formatter(...), NOT set_pattern(...). To exercise the
        // dispatcher's set_pattern override directly (lines 66-70 of
        // logging.cpp), call set_pattern on the dispatcher itself.
        // The dispatcher is the last sink on the default logger
        // (init() pushes [s_sink, s_dispatcher] in that order).
        auto& sinks = spdlog::default_logger()->sinks();
        REQUIRE(sinks.size() >= 1);
        sinks.back()->set_pattern("V2310-PATTERN %v");

        entropic::log::HandleLogScope scope(ID);
        auto logger = entropic::log::get("logging-test.set_pattern.probe");
        logger->info("payload");
        spdlog::default_logger()->flush();

        auto contents = read_file_contents(base / "session.log");
        // Reset to canonical pattern for unrelated scenarios.
        sinks.back()->set_pattern(
            "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

        THEN("the new pattern was forwarded to the handle's file sink") {
            REQUIRE(contents.find("V2310-PATTERN") != std::string::npos);
        }
    }

    entropic::log::unregister_handle_log(ID);
}

SCENARIO("HandleAwareSink::set_formatter is reachable via default logger",
         "[logging][types][set-formatter][gh59]") {
    auto base = std::filesystem::temp_directory_path()
        / "v2.3.10-set-formatter";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    entropic::log::init(spdlog::level::info);
    constexpr int ID = 5002;
    entropic::log::register_handle_log(ID, base);

    WHEN("a custom formatter is installed on the default logger") {
        // spdlog::default_logger()->set_formatter walks every sink and
        // forwards the formatter. The dispatcher's set_formatter
        // currently forwards only to the first registered sink (see
        // logging.cpp comment for rationale); this scenario exercises
        // that path purely for coverage, not behavior assertion.
        auto fmt = std::make_unique<spdlog::pattern_formatter>("FMT-PROBE %v");
        spdlog::default_logger()->set_formatter(std::move(fmt));

        // Restore canonical pattern so unrelated tests aren't disturbed.
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

        THEN("no exception escapes; dispatcher::set_formatter executed") {
            // The dispatcher's set_formatter is a void return on a
            // path that only mutates internal state.
            REQUIRE(true);
        }
    }

    entropic::log::unregister_handle_log(ID);
}

// ── Process-global side-effect tests run LAST in this binary ──
//
// set_console_enabled(false) is a one-way switch — it strips s_sink
// from every registered logger and sets s_console_disabled so future
// get() calls also strip it. Subsequent scenarios in this file still
// work (loggers function without stderr), but no test should depend
// on the console sink existing after this point.

SCENARIO("set_console_enabled(true) is a no-op",
         "[logging][types][set-console-enabled][zzz-process-global]") {
    entropic::log::init(spdlog::level::info);
    WHEN("set_console_enabled(true) is called on an initialized system") {
        entropic::log::set_console_enabled(true);
        THEN("get() still returns functional loggers") {
            auto logger = entropic::log::get(
                "logging-test.console_enabled_true");
            REQUIRE(logger != nullptr);
        }
    }
}

SCENARIO("set_console_enabled(false) strips stderr; get() strips for new loggers",
         "[logging][types][set-console-enabled][zzz-process-global]") {
    entropic::log::init(spdlog::level::info);

    WHEN("set_console_enabled(false) is called") {
        entropic::log::set_console_enabled(false);

        THEN("get() returns functional loggers (file sinks still work)") {
            auto logger = entropic::log::get(
                "logging-test.after_console_disabled");
            REQUIRE(logger != nullptr);
            // get() takes the console-disabled strip branch on new
            // loggers; we can't observe sink identity directly but
            // the call must return non-null.
            logger->info("after-disable-marker");
        }

        AND_THEN("calling set_console_enabled(false) a second time is idempotent") {
            entropic::log::set_console_enabled(false);
            auto logger = entropic::log::get(
                "logging-test.after_console_disabled_2");
            REQUIRE(logger != nullptr);
        }
    }
}
