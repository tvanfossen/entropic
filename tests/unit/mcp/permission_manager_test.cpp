// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_permission_manager.cpp
 * @brief PermissionManager unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/permission_manager.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Deny takes precedence", "[permission_manager]") {
    PermissionManager pm(
        {"filesystem.*"},           // allow
        {"filesystem.write_file"}   // deny
    );
    REQUIRE(pm.is_denied(
        "filesystem.write_file", "filesystem.write_file:test.txt"));
    REQUIRE(pm.is_allowed(
        "filesystem.read_file", "filesystem.read_file:test.txt"));
}

TEST_CASE("Glob pattern matching", "[permission_manager]") {
    PermissionManager pm({"filesystem.*"}, {});
    REQUIRE(pm.is_allowed(
        "filesystem.read_file", "filesystem.read_file:x"));
    REQUIRE(pm.is_allowed(
        "filesystem.write_file", "filesystem.write_file:x"));
    REQUIRE_FALSE(pm.is_allowed(
        "bash.execute", "bash.execute:ls"));
}

TEST_CASE("Tool level pattern", "[permission_manager]") {
    PermissionManager pm({"filesystem.read_file"}, {});
    REQUIRE(pm.is_allowed(
        "filesystem.read_file", "filesystem.read_file:any"));
    REQUIRE_FALSE(pm.is_allowed(
        "filesystem.write_file", "filesystem.write_file:any"));
}

TEST_CASE("Argument level pattern", "[permission_manager]") {
    PermissionManager pm({"bash.execute:python*"}, {});
    REQUIRE(pm.is_allowed(
        "bash.execute", "bash.execute:python3 script.py"));
    REQUIRE_FALSE(pm.is_allowed(
        "bash.execute", "bash.execute:rm -rf /"));
}

TEST_CASE("Add permission runtime", "[permission_manager]") {
    PermissionManager pm;
    REQUIRE_FALSE(pm.is_allowed(
        "git.commit", "git.commit:*"));
    pm.add_permission("git.commit", true);
    REQUIRE(pm.is_allowed(
        "git.commit", "git.commit:*"));
}

TEST_CASE("Empty lists — nothing denied", "[permission_manager]") {
    PermissionManager pm;
    REQUIRE_FALSE(pm.is_denied(
        "anything.tool", "anything.tool:arg"));
}
