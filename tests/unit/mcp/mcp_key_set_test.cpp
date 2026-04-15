// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_mcp_key_set.cpp
 * @brief MCPKeySet unit tests.
 * @version 1.9.4
 */

#include <entropic/mcp/mcp_key_set.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

TEST_CASE("MCPKeySet empty denies all", "[mcp][key_set]") {
    MCPKeySet ks;
    REQUIRE_FALSE(ks.has_access("filesystem.read_file",
                                MCPAccessLevel::READ));
    REQUIRE_FALSE(ks.has_access("bash.execute",
                                MCPAccessLevel::WRITE));
    REQUIRE(ks.size() == 0);
}

TEST_CASE("MCPKeySet exact match READ", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.read_file", MCPAccessLevel::READ);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::READ));
}

TEST_CASE("MCPKeySet exact match WRITE", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.write_file", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.write_file",
                          MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet READ does not authorize WRITE",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::READ);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::READ));
    REQUIRE_FALSE(ks.has_access("filesystem.write_file",
                                MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet WRITE implies READ", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::READ));
    REQUIRE(ks.has_access("filesystem.write_file",
                          MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet server wildcard match",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::READ));
    REQUIRE(ks.has_access("filesystem.edit_file",
                          MCPAccessLevel::WRITE));
    REQUIRE_FALSE(ks.has_access("bash.execute",
                                MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet full wildcard match", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("anything.anything",
                          MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet most specific wins: exact over wildcard",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::READ);
    ks.grant("filesystem.write_file", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.write_file",
                          MCPAccessLevel::WRITE));
    REQUIRE_FALSE(ks.has_access("filesystem.edit_file",
                                MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet most specific wins: server over full",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("*", MCPAccessLevel::READ);
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.write_file",
                          MCPAccessLevel::WRITE));
    REQUIRE_FALSE(ks.has_access("bash.execute",
                                MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet revoke removes access",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::READ));
    ks.revoke("filesystem.*");
    REQUIRE_FALSE(ks.has_access("filesystem.read_file",
                                MCPAccessLevel::READ));
}

TEST_CASE("MCPKeySet revoke nonexistent returns false",
          "[mcp][key_set]") {
    MCPKeySet ks;
    REQUIRE_FALSE(ks.revoke("nonexistent.*"));
}

TEST_CASE("MCPKeySet grant updates level", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::READ);
    REQUIRE_FALSE(ks.has_access("filesystem.write_file",
                                MCPAccessLevel::WRITE));
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.has_access("filesystem.write_file",
                          MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet grant can downgrade", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    ks.grant("filesystem.*", MCPAccessLevel::READ);
    REQUIRE_FALSE(ks.has_access("filesystem.write_file",
                                MCPAccessLevel::WRITE));
}

TEST_CASE("MCPKeySet list returns all keys",
          "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    ks.grant("git.*", MCPAccessLevel::READ);
    ks.grant("bash.execute", MCPAccessLevel::WRITE);
    auto keys = ks.list();
    REQUIRE(keys.size() == 3);
}

TEST_CASE("MCPKeySet size matches grants", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("a.*", MCPAccessLevel::READ);
    ks.grant("b.*", MCPAccessLevel::WRITE);
    REQUIRE(ks.size() == 2);
}

TEST_CASE("MCPKeySet clear removes all", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    ks.grant("git.*", MCPAccessLevel::READ);
    ks.clear();
    REQUIRE(ks.size() == 0);
}

TEST_CASE("MCPKeySet serialize roundtrip", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    ks.grant("git.status", MCPAccessLevel::READ);
    auto json = ks.serialize();

    MCPKeySet ks2;
    REQUIRE(ks2.deserialize(json));
    REQUIRE(ks2.size() == 2);
    REQUIRE(ks2.has_access("filesystem.read_file",
                           MCPAccessLevel::READ));
    REQUIRE(ks2.has_access("git.status",
                           MCPAccessLevel::READ));
}

TEST_CASE("MCPKeySet serialize format", "[mcp][key_set]") {
    MCPKeySet ks;
    ks.grant("filesystem.*", MCPAccessLevel::WRITE);
    auto json_str = ks.serialize();
    auto arr = nlohmann::json::parse(json_str);
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 1);
    REQUIRE(arr[0]["pattern"] == "filesystem.*");
    REQUIRE(arr[0]["level"] == "WRITE");
}

TEST_CASE("MCPKeySet deserialize invalid JSON",
          "[mcp][key_set]") {
    MCPKeySet ks;
    REQUIRE_FALSE(ks.deserialize("not json"));
}

TEST_CASE("MCPKeySet deserialize skips invalid entries",
          "[mcp][key_set]") {
    auto json = R"([
        {"pattern": "filesystem.*", "level": "WRITE"},
        {"pattern": "bad_entry"},
        {"pattern": "git.*", "level": "BOGUS"},
        {"pattern": "bash.*", "level": "READ"}
    ])";
    MCPKeySet ks;
    REQUIRE(ks.deserialize(json));
    REQUIRE(ks.size() == 2);
    REQUIRE(ks.has_access("filesystem.read_file",
                          MCPAccessLevel::WRITE));
    REQUIRE(ks.has_access("bash.execute",
                          MCPAccessLevel::READ));
}
