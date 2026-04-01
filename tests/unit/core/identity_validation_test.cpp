/**
 * @file test_identity_validation.cpp
 * @brief Identity name and field validation tests.
 * @version 1.9.6
 */

#include <entropic/core/identity_manager.h>
#include <entropic/inference/grammar_registry.h>
#include <entropic/mcp/mcp_authorization.h>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
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
 * @brief Build MCPKeyInterface (no-op for validation tests).
 * @return Empty callback interface.
 * @internal
 * @version 1.9.6
 */
static MCPKeyInterface make_noop_mcp_iface() {
    return {};
}

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
    cfg.system_prompt = "Test prompt";
    cfg.focus = {"test"};
    return cfg;
}

/**
 * @brief Build a wired IdentityManager for validation tests.
 * @param grammar_reg GrammarRegistry to wire.
 * @return Configured IdentityManager.
 * @internal
 * @version 1.9.6
 */
static std::unique_ptr<IdentityManager> make_mgr(
    GrammarRegistry& grammar_reg) {
    IdentityManagerConfig mgr_cfg;
    auto mgr = std::make_unique<IdentityManager>(mgr_cfg);
    mgr->set_grammar_interface(make_grammar_iface(grammar_reg));
    mgr->set_mcp_interface(make_noop_mcp_iface());
    return mgr;
}

// ── Name validation ──────────────────────────────────────

TEST_CASE("valid names accepted", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config("a")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("npc_guard")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("tutor-math")) == ENTROPIC_OK);
    REQUIRE(mgr->create(make_config("x123")) == ENTROPIC_OK);
}

TEST_CASE("uppercase name rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config("NPC_Guard"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("name with spaces rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config("npc guard"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("leading digit rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config("1npc"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("empty name rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config(""))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("name too long rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    std::string long_name(65, 'a');
    REQUIRE(mgr->create(make_config(long_name))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("reserved names rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    for (const auto& reserved : {"default", "none", "all", "router"}) {
        REQUIRE(mgr->create(make_config(reserved))
                == ENTROPIC_ERROR_INVALID_CONFIG);
    }
}

TEST_CASE("special chars rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    REQUIRE(mgr->create(make_config("npc.guard"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
    REQUIRE(mgr->create(make_config("npc/guard"))
            == ENTROPIC_ERROR_INVALID_CONFIG);
}

// ── Focus validation ─────────────────────────────────────

TEST_CASE("empty focus rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    IdentityConfig cfg;
    cfg.name = "npc_guard";
    cfg.system_prompt = "Test";
    cfg.focus = {};
    REQUIRE(mgr->create(cfg) == ENTROPIC_ERROR_INVALID_CONFIG);
}

// ── Grammar ID validation ────────────────────────────────

TEST_CASE("nonexistent grammar_id rejected",
          "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    cfg.grammar_id = "nonexistent";
    REQUIRE(mgr->create(cfg) == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("empty grammar_id accepted", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    cfg.grammar_id = "";
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
}

TEST_CASE("valid grammar_id accepted", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    grammar_reg.register_grammar("chess", "root ::= [a-h]");
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    cfg.grammar_id = "chess";
    REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
}

// ── Role type validation ─────────────────────────────────

TEST_CASE("invalid role_type rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    cfg.role_type = "invalid_role";
    REQUIRE(mgr->create(cfg) == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("valid role types accepted", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    for (const auto& [name, role] :
         std::vector<std::pair<std::string, std::string>>{
             {"a", "front_office"},
             {"b", "back_office"},
             {"c", "utility"}}) {
        auto cfg = make_config(name);
        cfg.role_type = role;
        REQUIRE(mgr->create(cfg) == ENTROPIC_OK);
    }
}

// ── Phase validation ─────────────────────────────────────

TEST_CASE("phase empty name rejected", "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    PhaseConfig phase;
    cfg.phases[""] = phase;
    REQUIRE(mgr->create(cfg) == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("phase negative tokens rejected",
          "[identity_validation]") {
    GrammarRegistry grammar_reg;
    auto mgr = make_mgr(grammar_reg);

    auto cfg = make_config("npc_guard");
    PhaseConfig phase;
    phase.max_output_tokens = -1;
    cfg.phases["planning"] = phase;
    REQUIRE(mgr->create(cfg) == ENTROPIC_ERROR_INVALID_CONFIG);
}
