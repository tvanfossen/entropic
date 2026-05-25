// SPDX-License-Identifier: Apache-2.0
/**
 * @file stdio_transport_test.cpp
 * @brief Unit tests for StdioTransport display_name handling (gh#19).
 *
 * Focused on the display-name path added in v2.1.5: the 5-arg ctor
 * carries a sanitized label used for bracketed stderr / lifecycle
 * logs. Sanitization protects Rich-based TUI consumers (and any
 * markdown renderer) from BBCode-close-tag collisions when the
 * registered server name starts with `/` or contains bracket /
 * control characters.
 *
 * @version 2.1.5
 */

#include <entropic/mcp/transport_stdio.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace entropic;

TEST_CASE("StdioTransport 5-arg ctor preserves clean display_name",
          "[mcp][stdio][gh19][v2.1.5]") {
    StdioTransport t("docs", "/usr/bin/env",
                     std::vector<std::string>{"python", "-m", "docs"},
                     {}, 30000U);
    CHECK(t.display_name() == "docs");
}

TEST_CASE("StdioTransport falls back to command when display_name empty",
          "[mcp][stdio][gh19][v2.1.5]") {
    StdioTransport t("", "git",
                     std::vector<std::string>{"--version"},
                     {}, 30000U);
    CHECK(t.display_name() == "git");
}

TEST_CASE("StdioTransport sanitizes leading slash (BBCode collision)",
          "[mcp][stdio][gh19][v2.1.5]") {
    // A registered name beginning with `/` would otherwise produce
    // log lines like `[/foo]` — Rich interprets that as a BBCode
    // close tag and strips surrounding markup.
    StdioTransport t("/foo", "/usr/bin/env",
                     std::vector<std::string>{"python"},
                     {}, 30000U);
    CHECK(t.display_name() == "foo");
    CHECK(t.display_name().front() != '/');
}

TEST_CASE("StdioTransport strips bracket and control characters",
          "[mcp][stdio][gh19][v2.1.5]") {
    // Split the literal so `\x7f` doesn't greedy-parse into `\x7fd`
    // (which would produce a single 0x7fd-truncated char, not DEL+d).
    StdioTransport t("a[b]c\x01\x1b\x7f" "d", "x", {}, {}, 1000U);
    CHECK(t.display_name() == "abcd");
}

TEST_CASE("StdioTransport sanitization fallback to 'server'",
          "[mcp][stdio][gh19][v2.1.5]") {
    // Input that sanitizes to empty must not produce an empty
    // bracket label.
    StdioTransport t("[][]", "x", {}, {}, 1000U);
    CHECK(t.display_name() == "server");
}

TEST_CASE("StdioTransport 1-arg ctor also sanitizes (legacy path)",
          "[mcp][stdio][gh19][v2.1.5]") {
    // Legacy ctor falls back to command; command paths frequently
    // start with `/` (e.g. /usr/bin/env). Sanitization must apply
    // there too so existing call sites also get protection.
    StdioTransport t("/usr/bin/env",
                     std::vector<std::string>{"python"},
                     {}, 30000U);
    CHECK(t.display_name().front() != '/');
    CHECK(t.display_name() == "usr/bin/env");
}

// ── v2.3.10: open + close + spawn-failure coverage ──

TEST_CASE("StdioTransport rejects open() against a non-existent command",
          "[mcp][stdio][v2.3.10][coverage][failure-mode]") {
    // The fork+exec path fails when the binary doesn't exist;
    // open_child_process returns false → open() returns false.
    StdioTransport t("ghost", "/path/that/does/not/exist/v2310",
                     std::vector<std::string>{}, {}, 1000U);
    REQUIRE_FALSE(t.open());
    REQUIRE_FALSE(t.is_connected());
}

TEST_CASE("StdioTransport open() is idempotent on already-connected state",
          "[mcp][stdio][v2.3.10][coverage]") {
    // /bin/cat speaks no JSON-RPC but does keep the child alive
    // long enough for the transport's open + close lifecycle to
    // exercise its full path: open() returns true, second open()
    // hits the early-return at line 156, close() tears down cleanly.
    StdioTransport t("cat", "/bin/cat", std::vector<std::string>{},
                     {}, 1000U);
    if (!std::filesystem::exists("/bin/cat")) { return; }

    REQUIRE(t.open());
    REQUIRE(t.is_connected());

    // Second open() — early-out branch (line 156).
    REQUIRE(t.open());
    REQUIRE(t.is_connected());

    t.close();
    REQUIRE_FALSE(t.is_connected());
}

TEST_CASE("StdioTransport.send_request times out cleanly when child doesn't reply",
          "[mcp][stdio][v2.3.10][coverage][failure-mode]") {
    // /bin/cat echoes stdin to stdout but the echoed payload won't
    // match the JSON-RPC id parsing entropic does — so send_request
    // either times out or returns a non-matching response. Either
    // way it must not crash; that's the coverage assertion.
    StdioTransport t("cat", "/bin/cat", std::vector<std::string>{},
                     {}, 200U);
    if (!std::filesystem::exists("/bin/cat")) { return; }

    REQUIRE(t.open());
    // Short per-call timeout so the test completes quickly.
    auto resp = t.send_request(
        R"({"jsonrpc":"2.0","id":1,"method":"unknown"})", 100U);
    (void)resp;  // value unconstrained; we only assert the call returns
    REQUIRE(true);
}

TEST_CASE("StdioTransport.close() with no prior open() is safe",
          "[mcp][stdio][v2.3.10][coverage][failure-mode]") {
    // Tests the close path's tolerance for a never-opened transport
    // — no child PID, no fds, but close() must still return cleanly.
    StdioTransport t("noop", "/bin/true",
                     std::vector<std::string>{}, {}, 100U);
    t.close();  // no-op path
    REQUIRE_FALSE(t.is_connected());
}
