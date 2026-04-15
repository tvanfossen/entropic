// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_mcp_authorization.cpp
 * @brief MCPAuthorizationManager unit tests.
 * @version 1.9.4
 */

#include <entropic/mcp/mcp_authorization.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

TEST_CASE("Unregistered identity passes authorization",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    REQUIRE(mgr.check_access("unknown", "filesystem.read_file",
                             MCPAccessLevel::WRITE));
}

TEST_CASE("Registered empty identity denies all",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    REQUIRE_FALSE(mgr.check_access("eng",
        "filesystem.read_file", MCPAccessLevel::READ));
}

TEST_CASE("Grant then check succeeds",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    REQUIRE(mgr.grant("eng", "filesystem.*",
                      MCPAccessLevel::WRITE) == ENTROPIC_OK);
    REQUIRE(mgr.check_access("eng", "filesystem.read_file",
                             MCPAccessLevel::READ));
}

TEST_CASE("Revoke then check fails",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    mgr.grant("eng", "filesystem.*", MCPAccessLevel::WRITE);
    mgr.revoke("eng", "filesystem.*");
    REQUIRE_FALSE(mgr.check_access("eng",
        "filesystem.read_file", MCPAccessLevel::READ));
}

TEST_CASE("Grant from valid granter succeeds",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("lead");
    mgr.register_identity("eng");
    mgr.grant("lead", "*", MCPAccessLevel::WRITE);
    auto rc = mgr.grant_from("lead", "eng",
                             "filesystem.*",
                             MCPAccessLevel::WRITE);
    REQUIRE(rc == ENTROPIC_OK);
    REQUIRE(mgr.check_access("eng", "filesystem.read_file",
                             MCPAccessLevel::WRITE));
}

TEST_CASE("Grant from insufficient level denied",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("lead");
    mgr.register_identity("eng");
    mgr.grant("lead", "filesystem.*", MCPAccessLevel::READ);
    auto rc = mgr.grant_from("lead", "eng",
                             "filesystem.*",
                             MCPAccessLevel::WRITE);
    REQUIRE(rc == ENTROPIC_ERROR_PERMISSION_DENIED);
}

TEST_CASE("Grant from missing key denied",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("lead");
    mgr.register_identity("eng");
    // lead has no keys at all
    auto rc = mgr.grant_from("lead", "eng",
                             "filesystem.*",
                             MCPAccessLevel::READ);
    REQUIRE(rc == ENTROPIC_ERROR_PERMISSION_DENIED);
}

TEST_CASE("Grant from unregistered granter returns NOT_FOUND",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    auto rc = mgr.grant_from("nobody", "eng",
                             "filesystem.*",
                             MCPAccessLevel::READ);
    REQUIRE(rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
}

TEST_CASE("Grant from unregistered grantee returns NOT_FOUND",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("lead");
    mgr.grant("lead", "*", MCPAccessLevel::WRITE);
    auto rc = mgr.grant_from("lead", "nobody",
                             "filesystem.*",
                             MCPAccessLevel::READ);
    REQUIRE(rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
}

TEST_CASE("List keys returns correct vector",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    mgr.grant("eng", "filesystem.*", MCPAccessLevel::WRITE);
    mgr.grant("eng", "git.*", MCPAccessLevel::READ);
    auto keys = mgr.list_keys("eng");
    REQUIRE(keys.size() == 2);
}

TEST_CASE("List keys unregistered returns empty",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    auto keys = mgr.list_keys("unknown");
    REQUIRE(keys.empty());
}

TEST_CASE("Serialize all roundtrip",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("lead");
    mgr.register_identity("eng");
    mgr.grant("lead", "*", MCPAccessLevel::WRITE);
    mgr.grant("eng", "filesystem.*", MCPAccessLevel::READ);

    auto json = mgr.serialize_all();

    MCPAuthorizationManager mgr2;
    REQUIRE(mgr2.deserialize_all(json));
    REQUIRE(mgr2.check_access("lead", "anything",
                              MCPAccessLevel::WRITE));
    REQUIRE(mgr2.check_access("eng", "filesystem.read_file",
                              MCPAccessLevel::READ));
    REQUIRE_FALSE(mgr2.check_access("eng",
        "filesystem.write_file", MCPAccessLevel::WRITE));
}

TEST_CASE("Unregister disables enforcement",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    mgr.register_identity("eng");
    REQUIRE_FALSE(mgr.check_access("eng",
        "filesystem.read_file", MCPAccessLevel::READ));
    mgr.unregister_identity("eng");
    REQUIRE(mgr.check_access("eng",
        "filesystem.read_file", MCPAccessLevel::READ));
}

TEST_CASE("is_enforced correct",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    REQUIRE_FALSE(mgr.is_enforced("eng"));
    mgr.register_identity("eng");
    REQUIRE(mgr.is_enforced("eng"));
}

TEST_CASE("Grant to unregistered identity returns NOT_FOUND",
          "[mcp][authorization]") {
    MCPAuthorizationManager mgr;
    auto rc = mgr.grant("nobody", "filesystem.*",
                        MCPAccessLevel::WRITE);
    REQUIRE(rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
}
