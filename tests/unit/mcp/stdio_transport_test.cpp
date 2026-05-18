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
