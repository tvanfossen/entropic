// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_identity_manager.cpp
 * @brief IdentityManager unit tests — CRUD, validation, MCP keys, dirty flag.
 * @version 1.9.6
 */

#include <entropic/core/identity_manager.h>
#include <entropic/inference/grammar_registry.h>
#include <entropic/mcp/mcp_authorization.h>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace entropic;

// ── Callback wiring helpers ──────────────────────────────

/**
 * @brief Build GrammarValidationInterface wired to a real GrammarRegistry.
 * @param reg GrammarRegistry to wire.
 * @return Populated callback interface.
 * @internal
 * @version 1.9.6
 */
static GrammarValidationInterface make_grammar_iface(
    GrammarRegistry& reg) {
    GrammarValidationInterface iface;
    iface.has_grammar = [](const std::string& key,
                           void* ud) -> bool {
        return static_cast<GrammarRegistry*>(ud)->has(key);
    };
    iface.user_data = &reg;
    return iface;
}

/**
 * @brief Build MCPKeyInterface wired to a real MCPAuthorizationManager.
 * @param mgr MCPAuthorizationManager to wire.
 * @return Populated callback interface.
 * @internal
 * @version 1.9.6
 */
static MCPKeyInterface make_mcp_iface(MCPAuthorizationManager& mgr) {
    MCPKeyInterface iface;
    iface.register_identity = [](const std::string& name,
                                 void* ud) {
        static_cast<MCPAuthorizationManager*>(ud)
            ->register_identity(name);
    };
    iface.grant = [](const std::string& name,
                     const std::string& pattern, int level,
                     void* ud) {
        static_cast<MCPAuthorizationManager*>(ud)->grant(
            name, pattern, static_cast<MCPAccessLevel>(level));
    };
    iface.is_enforced = [](const std::string& name,
                           void* ud) -> bool {
        return static_cast<MCPAuthorizationManager*>(ud)
            ->is_enforced(name);
    };
    iface.unregister_identity = [](const std::string& name,
                                   void* ud) {
        static_cast<MCPAuthorizationManager*>(ud)
            ->unregister_identity(name);
    };
    iface.user_data = &mgr;
    return iface;
}

// ── Test helpers ─────────────────────────────────────────

/**
 * @brief Build a minimal valid dynamic identity config.
 * @param name Identity name.
 * @return IdentityConfig with required fields populated.
 * @internal
 * @version 1.9.6
 */
static IdentityConfig make_config(const std::string& name) {
    IdentityConfig cfg;
    cfg.name = name;
    cfg.system_prompt = "Test prompt for " + name;
    cfg.focus = {"test", "unit"};
    return cfg;
}

/**
 * @brief Build a static identity config.
 * @param name Identity name.
 * @return IdentityConfig with origin=STATIC.
 * @internal
 * @version 1.9.6
 */
static IdentityConfig make_static(const std::string& name) {
    auto cfg = make_config(name);
    cfg.origin = IdentityOrigin::STATIC;
    return cfg;
}

/**
 * @brief Build a fully wired IdentityManager for tests.
 * @param mgr_cfg Manager configuration.
 * @param grammar_reg GrammarRegistry to wire.
 * @param auth_mgr MCPAuthorizationManager to wire.
 * @return Configured IdentityManager.
 * @internal
 * @version 1.9.6
 */
static std::unique_ptr<IdentityManager> make_mgr(
    IdentityManagerConfig& mgr_cfg,
    GrammarRegistry& grammar_reg,
    MCPAuthorizationManager& auth_mgr) {
    auto mgr = std::make_unique<IdentityManager>(mgr_cfg);
    mgr->set_grammar_interface(make_grammar_iface(grammar_reg));
    mgr->set_mcp_interface(make_mcp_iface(auth_mgr));
    return mgr;
}

// ── load_static tests ────────────────────────────────────

TEST_CASE("load_static loads identities", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    std::vector<IdentityConfig> statics = {
        make_static("lead"), make_static("eng"), make_static("qa")};
    auto loaded = mgr->load_static(statics);

    REQUIRE(loaded == 3);
    REQUIRE(mgr->count() == 3);
    REQUIRE(mgr->count_dynamic() == 0);
    REQUIRE(mgr->has("lead"));
    REQUIRE(mgr->has("eng"));
    REQUIRE(mgr->has("qa"));
}

// ── create tests ─────────────────────────────────────────

TEST_CASE("create minimal dynamic identity", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    auto cfg = make_config("npc_guard");
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
    REQUIRE(mgr->has("npc_guard"));
    REQUIRE(mgr->count() == 1);
    REQUIRE(mgr->count_dynamic() == 1);

    const auto* id = mgr->get("npc_guard");
    REQUIRE(id != nullptr);
    REQUIRE(id->origin == IdentityOrigin::DYNAMIC);
    REQUIRE(id->system_prompt == "Test prompt for npc_guard");
}

TEST_CASE("create duplicate rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("npc_guard"))
            == ENTROPIC_ERROR_ALREADY_EXISTS);
}

TEST_CASE("create exceeds max rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    mgr_cfg.max_identities = 2;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("a")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("b")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("c"))
            == ENTROPIC_ERROR_LIMIT_REACHED);
}

TEST_CASE("create registers MCP keys", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    auto cfg = make_config("npc_guard");
    cfg.mcp_keys = {
        {"game.inventory", MCPAccessLevel::READ},
        {"game.dialog", MCPAccessLevel::WRITE}};
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);

    REQUIRE(auth_mgr.is_enforced("npc_guard"));
    REQUIRE(auth_mgr.check_access(
        "npc_guard", "game.inventory", MCPAccessLevel::READ));
    REQUIRE(auth_mgr.check_access(
        "npc_guard", "game.dialog", MCPAccessLevel::WRITE));
}

TEST_CASE("create with allow_dynamic=false rejected",
          "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    mgr_cfg.allow_dynamic = false;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("a"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

// ── update tests ─────────────────────────────────────────

TEST_CASE("update dynamic succeeds", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);

    auto updated = make_config("npc_guard");
    updated.system_prompt = "Updated prompt";
    updated.temperature = 0.5f;
    REQUIRE(mgr->update("npc_guard", updated) == ENTROPIC_OK);

    const auto* id = mgr->get("npc_guard");
    REQUIRE(id->system_prompt == "Updated prompt");
    REQUIRE(id->temperature == 0.5f);
}

TEST_CASE("update static rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->load_static({make_static("eng")});
    REQUIRE(mgr->update("eng", make_config("eng"))
            == ENTROPIC_ERROR_PERMISSION_DENIED);
}

TEST_CASE("update nonexistent rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->update("ghost", make_config("ghost"))
            == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
}

TEST_CASE("update refreshes MCP keys", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    auto cfg = make_config("npc_guard");
    cfg.mcp_keys = {{"old.tool", MCPAccessLevel::READ}};
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
    REQUIRE(auth_mgr.is_enforced("npc_guard"));

    auto updated = make_config("npc_guard");
    updated.mcp_keys = {{"new.tool", MCPAccessLevel::WRITE}};
    REQUIRE(mgr->update("npc_guard", updated) == ENTROPIC_OK);

    REQUIRE(auth_mgr.is_enforced("npc_guard"));
    REQUIRE(auth_mgr.check_access(
        "npc_guard", "new.tool", MCPAccessLevel::WRITE));
}

// ── destroy tests ────────────────────────────────────────

TEST_CASE("destroy dynamic succeeds", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);
    REQUIRE(mgr->destroy("npc_guard") == ENTROPIC_OK);
    REQUIRE_FALSE(mgr->has("npc_guard"));
    REQUIRE(mgr->count() == 0);
}

TEST_CASE("destroy static rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->load_static({make_static("eng")});
    REQUIRE(mgr->destroy("eng") == ENTROPIC_ERROR_PERMISSION_DENIED);
}

TEST_CASE("destroy nonexistent rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->destroy("ghost")
            == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
}

TEST_CASE("destroy in-use rejected", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->set_in_use_checker(
        [](const std::string& name, void*) -> bool {
            return name == "npc_guard";
        },
        nullptr);

    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);
    REQUIRE(mgr->destroy("npc_guard") == ENTROPIC_ERROR_IN_USE);
    REQUIRE(mgr->has("npc_guard"));
}

TEST_CASE("destroy unregisters MCP keys", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    auto cfg = make_config("npc_guard");
    cfg.mcp_keys = {{"game.dialog", MCPAccessLevel::WRITE}};
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
    REQUIRE(auth_mgr.is_enforced("npc_guard"));

    REQUIRE(mgr->destroy("npc_guard") == ENTROPIC_OK);
    REQUIRE_FALSE(auth_mgr.is_enforced("npc_guard"));
}

// ── get / has / list ─────────────────────────────────────

TEST_CASE("get returns config", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);
    const auto* id = mgr->get("npc_guard");
    REQUIRE(id != nullptr);
    REQUIRE(id->name == "npc_guard");
}

TEST_CASE("get nonexistent returns null", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE(mgr->get("ghost") == nullptr);
}

TEST_CASE("list includes all identities", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->load_static({make_static("lead"), make_static("eng")});
    mgr->create(make_config("npc_guard"));

    auto names = mgr->list();
    REQUIRE(names.size() == 3);
}

TEST_CASE("list_routable excludes non-routable",
          "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    auto routable = make_config("npc_guard");
    routable.routable = true;
    mgr->create(routable);

    auto hidden = make_config("utility_bot");
    hidden.routable = false;
    mgr->create(hidden);

    auto interstitial = make_config("interstitial_bot");
    interstitial.interstitial = true;
    mgr->create(interstitial);

    auto result = mgr->list_routable();
    REQUIRE(result.size() == 1);
    REQUIRE(result[0]->name == "npc_guard");
}

// ── count ────────────────────────────────────────────────

TEST_CASE("count total and dynamic", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->load_static({make_static("lead"), make_static("eng")});
    mgr->create(make_config("npc_guard"));
    mgr->create(make_config("npc_smith"));

    REQUIRE(mgr->count() == 4);
    REQUIRE(mgr->count_dynamic() == 2);

    mgr->destroy("npc_guard");
    REQUIRE(mgr->count() == 3);
    REQUIRE(mgr->count_dynamic() == 1);
}

// ── router dirty flag ────────────────────────────────────

TEST_CASE("router dirty on create", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    REQUIRE_FALSE(mgr->is_router_dirty());
    mgr->create(make_config("npc_guard"));
    REQUIRE(mgr->is_router_dirty());
}

TEST_CASE("router dirty cleared", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->create(make_config("npc_guard"));
    mgr->clear_router_dirty();
    REQUIRE_FALSE(mgr->is_router_dirty());
}

TEST_CASE("router dirty on destroy", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->create(make_config("npc_guard"));
    mgr->clear_router_dirty();
    mgr->destroy("npc_guard");
    REQUIRE(mgr->is_router_dirty());
}

TEST_CASE("router dirty on update", "[identity_manager]") {
    GrammarRegistry grammar_reg;
    MCPAuthorizationManager auth_mgr;
    IdentityManagerConfig mgr_cfg;
    auto mgr = make_mgr(mgr_cfg, grammar_reg, auth_mgr);

    mgr->create(make_config("npc_guard"));
    mgr->clear_router_dirty();

    auto updated = make_config("npc_guard");
    updated.focus = {"new_focus"};
    mgr->update("npc_guard", updated);
    REQUIRE(mgr->is_router_dirty());
}
