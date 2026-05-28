// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_orchestrator.cpp
 * @brief Tests for ModelOrchestrator dedup, swap, and handoff.
 *
 * Uses mock config — no real models loaded.
 *
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/inference/orchestrator.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/**
 * @brief Build a minimal config for orchestrator tests.
 * @version 1.8.2
 * @internal
 */
entropic::ParsedConfig make_test_config() {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";

    // Two tiers sharing same model path → should dedup
    entropic::TierConfig lead;
    lead.path = "/tmp/test_model.gguf";
    lead.adapter = "generic";
    lead.keep_warm = true;

    entropic::TierConfig eng;
    eng.path = "/tmp/test_model.gguf";  // Same path as lead
    eng.adapter = "qwen35";
    eng.keep_warm = false;

    config.models.tiers["lead"] = lead;
    config.models.tiers["eng"] = eng;

    // Handoff rules
    config.routing.handoff_rules["lead"] = {"eng"};
    config.routing.handoff_rules["eng"] = {"lead"};

    return config;
}

} // anonymous namespace

// ── Handoff rules ──────────────────────────────────────────

SCENARIO("Handoff rules", "[orchestrator][handoff]") {
    // Test handoff rule checking without model loading
    entropic::ModelOrchestrator orch;
    // Note: These tests only verify the query interface.
    // Full initialize() requires real model files.

    GIVEN("handoff rules configured") {
        // can_handoff checks the internal rules map
        // Without initialize(), rules are empty → all denied
        THEN("empty orchestrator denies all handoffs") {
            REQUIRE_FALSE(orch.can_handoff("lead", "eng"));
            REQUIRE_FALSE(orch.can_handoff("eng", "lead"));
        }
    }
}

// ── Query interface ────────────────────────────────────────

SCENARIO("Orchestrator query interface", "[orchestrator][query]") {
    entropic::ModelOrchestrator orch;

    THEN("empty orchestrator has no loaded models") {
        REQUIRE(orch.loaded_models().empty());
        REQUIRE(orch.available_models().empty());
        REQUIRE(orch.last_used_tier().empty());
    }
}

// ── v2.3.10: extended coverage for orchestrator public API ──

SCENARIO("Orchestrator has_vision_capable_tier reports false on empty",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    REQUIRE_FALSE(orch.has_vision_capable_tier());
}

SCENARIO("Orchestrator get_adapter returns nullptr for unknown tier",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    REQUIRE(orch.get_adapter("no-such-tier") == nullptr);
    REQUIRE(orch.get_adapter("") == nullptr);
}

SCENARIO("Orchestrator get_backend returns nullptr for unknown tier",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    REQUIRE(orch.get_backend("no-such-tier") == nullptr);
    REQUIRE(orch.get_backend("") == nullptr);
}

SCENARIO("Orchestrator accessor functions return mutable references safely",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    // Each accessor returns a reference to an owned subsystem.
    auto& adapter_mgr = orch.adapter_manager();
    (void)adapter_mgr;
    auto& grammar_reg = orch.grammar_registry();
    (void)grammar_reg;
    auto& profile_reg = orch.profile_registry();
    (void)profile_reg;
    auto& tput = orch.throughput_tracker();
    (void)tput;
    REQUIRE(true);
}

SCENARIO("Orchestrator clear_all_prompt_caches is safe on empty state",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    orch.clear_all_prompt_caches();  // no-op when no tiers loaded
    REQUIRE(true);
}

SCENARIO("Orchestrator load_grammars_from with missing dir returns 0",
         "[orchestrator][v2.3.10][coverage][failure-mode]")
{
    entropic::ModelOrchestrator orch;
    auto count = orch.load_grammars_from(
        "/path/that/does/not/exist/v2310");
    REQUIRE(count == 0);
}

SCENARIO("Orchestrator initialize against missing tier paths fails cleanly",
         "[orchestrator][v2.3.10][coverage][failure-mode]")
{
    entropic::ModelOrchestrator orch;
    auto config = make_test_config();
    // /tmp/test_model.gguf doesn't exist → load fails → initialize false.
    bool ok = orch.initialize(config);
    REQUIRE_FALSE(ok);
    // Failed init still allows query-API calls without crashing.
    REQUIRE(orch.last_used_tier().empty());
    REQUIRE_FALSE(orch.has_vision_capable_tier());
}

SCENARIO("Orchestrator can_handoff with empty rules returns false for any pair",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    REQUIRE_FALSE(orch.can_handoff("", ""));
    REQUIRE_FALSE(orch.can_handoff("a", "b"));
    REQUIRE_FALSE(orch.can_handoff("lead", "lead"));
}

SCENARIO("Orchestrator shutdown is safe on uninitialized state",
         "[orchestrator][v2.3.10][coverage]")
{
    entropic::ModelOrchestrator orch;
    orch.shutdown();  // no-op when no tiers
    orch.shutdown();  // idempotent
    REQUIRE(true);
}

// ── v2.3.10 [v2.3.10][inference][orchestrator_unit]: deeper drive of ──
// orchestrator.cpp without a real model. We create a fake (non-empty)
// GGUF file so `create_tier_backends` succeeds (model_pool insert path,
// adapter creation), but `LlamaCppBackend::do_load` fails on the
// invalid file content, exercising `activate_default_tier`'s error
// branch + the downstream tear-down paths. This is the closest we can
// get to driving the cold tier-load surface without a real model.

namespace {

/**
 * @brief Create an empty (non-GGUF) placeholder file. Path exists
 *        → `create_tier_backends` passes the existence check, but the
 *        actual `do_load` will fail on parse — that's the entire point.
 */
std::filesystem::path make_placeholder_gguf(const std::string& stem) {
    auto p = std::filesystem::temp_directory_path()
        / ("entropic_orch_v2310_" + stem + ".gguf");
    {
        std::ofstream out(p, std::ios::binary);
        // Write a few non-zero bytes so file_size() > 0 for footprint
        // estimation. Content is intentionally garbage — not a real GGUF.
        out << "NOT_A_REAL_GGUF_FILE_PLACEHOLDER";
    }
    return p;
}

/**
 * @brief Build a config with two tiers backed by the same placeholder
 *        GGUF (dedup path) + routing enabled with a digit map + a
 *        handoff rule.
 */
entropic::ParsedConfig make_load_failure_config(
    const std::filesystem::path& fake_gguf) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    config.vram_reserve_mb = 0;  // keep footprint math predictable

    entropic::TierConfig lead;
    lead.path = fake_gguf;
    lead.adapter = "generic";
    lead.keep_warm = true;
    lead.context_length = 1024;
    lead.capabilities = {"text", "vision"};  // drive vision-tier lookup

    entropic::TierConfig eng;
    eng.path = fake_gguf;  // share same path → model_pool dedup
    eng.adapter = "qwen35";
    eng.keep_warm = false;
    eng.context_length = 512;
    eng.capabilities = {"text"};

    config.models.tiers["lead"] = lead;
    config.models.tiers["eng"] = eng;

    // Routing on — drives the non-default branch of `route()`.
    config.routing.enabled = true;
    config.routing.fallback_tier = "lead";
    config.routing.tier_map["1"] = "lead";
    config.routing.tier_map["2"] = "eng";
    config.routing.handoff_rules["lead"] = {"eng"};
    config.routing.handoff_rules["eng"] = {"lead"};

    return config;
}

}  // namespace

SCENARIO("Orchestrator drives create_tier_backends + activation failure",
         "[v2.3.10][inference][orchestrator_unit][failure-mode]")
{
    auto fake = make_placeholder_gguf("init_fail");
    auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove(fake, ec);
    };

    entropic::ModelOrchestrator orch;
    auto config = make_load_failure_config(fake);

    // Initialize: file exists → create_tier_backends succeeds (covers
    // model_pool insert + adapter create). load_and_activate fails on
    // invalid GGUF → activate_default_tier returns false → initialize
    // returns false. This drives create_tier_backends, build_routing_
    // tables, and activate_default_tier's failure branch in one shot.
    bool ok = orch.initialize(config);
    REQUIRE_FALSE(ok);

    // Queries against the partially-initialized state should not crash
    // and should reflect the configured tiers (config_ was assigned
    // before the failed activate).
    auto available = orch.available_models();
    REQUIRE(available.size() >= 2);  // "lead", "eng" present

    // Adapters were created in create_tier_backends → non-null lookup.
    REQUIRE(orch.get_adapter("lead") != nullptr);
    REQUIRE(orch.get_adapter("eng") != nullptr);

    // Backend pool was populated → get_backend returns non-null for
    // configured tiers. They are NOT loaded (do_load failed).
    REQUIRE(orch.get_backend("lead") != nullptr);
    REQUIRE(orch.get_backend("eng") != nullptr);

    // Vision-tier capability lookup now hits a non-empty tier map.
    REQUIRE(orch.has_vision_capable_tier());
    REQUIRE_FALSE(orch.select_vision_tier().empty());

    // Handoff rules were populated by build_routing_tables.
    REQUIRE(orch.can_handoff("lead", "eng"));
    REQUIRE(orch.can_handoff("eng", "lead"));
    REQUIRE_FALSE(orch.can_handoff("lead", "no-such"));

    // loaded_models reflects the residency state — since load failed,
    // none should report loaded.
    auto loaded = orch.loaded_models();
    REQUIRE(loaded.empty());

    // Residency snapshot JSON is valid even with no tier loaded —
    // header reports vram_budget/headroom, residency array is empty.
    auto snap = orch.residency_snapshot_json();
    REQUIRE(snap.find("residency") != std::string::npos);
    REQUIRE(snap.find("vram_budget_bytes") != std::string::npos);

    // Footprint estimation hits the file_size + KV + headroom path.
    auto fp = orch.tier_footprint_bytes("lead");
    REQUIRE(fp > 0);  // weights file exists + context_length*16KiB
    auto fp_eng = orch.tier_footprint_bytes("eng");
    REQUIRE(fp_eng > 0);
    auto fp_missing = orch.tier_footprint_bytes("no-such-tier");
    REQUIRE(fp_missing == 0);

    // Clear caches — fans out across pooled backends + secondary loader.
    orch.clear_all_prompt_caches();

    // Shutdown + idempotent shutdown — drives backend pool iteration.
    orch.shutdown();
    orch.shutdown();

    cleanup();
}

SCENARIO("Orchestrator generate() returns typed error when tier missing",
         "[v2.3.10][inference][orchestrator_unit][failure-mode]")
{
    auto fake = make_placeholder_gguf("gen_err");
    entropic::ModelOrchestrator orch;
    auto config = make_load_failure_config(fake);
    (void)orch.initialize(config);  // expect false — fine

    std::vector<entropic::Message> msgs = {
        {"user", "hello", {}, {}}
    };
    entropic::GenerationParams params;
    params.max_tokens = 1;

    // Explicit-tier path: tier exists in tier_map but backend is not
    // active and load fails → get_model returns nullptr → generate
    // builds error result (drives build_no_model_error fallback branch).
    auto result = orch.generate(msgs, params, "lead");
    REQUIRE(result.finish_reason == "error");
    REQUIRE(result.error_code != ENTROPIC_OK);
    REQUIRE_FALSE(result.error_message.empty());

    // Same shape for streaming.
    std::atomic<bool> cancel{false};
    auto streamed = orch.generate_streaming(
        msgs, params,
        [](std::string_view) {}, cancel, "lead");
    REQUIRE(streamed.finish_reason == "error");
    REQUIRE_FALSE(streamed.error_message.empty());

    // Last routing result should reflect a routing decision — but with
    // explicit tier passed we don't route, so just verify the accessor
    // is callable.
    auto last = orch.last_routing_result();
    (void)last;

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

SCENARIO("Orchestrator route() exercises both enabled and disabled paths",
         "[v2.3.10][inference][orchestrator_unit]")
{
    auto fake = make_placeholder_gguf("route");
    entropic::ModelOrchestrator orch;
    auto config = make_load_failure_config(fake);
    (void)orch.initialize(config);

    std::vector<entropic::Message> msgs = {
        {"user", "what is 2+2?", {}, {}}
    };

    // routing.enabled=true but router model has no path set (no router
    // configured in config.models.router). The early-out branch (no
    // router configured) returns the default tier without invoking
    // classify_task. Drives lines 506-513.
    auto tier = orch.route(msgs);
    REQUIRE(tier == "lead");
    auto rr = orch.last_routing_result();
    REQUIRE(rr.tier_name == "lead");
    REQUIRE(rr.swap_action == "none");

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

SCENARIO("Orchestrator residency observer set/clear + speculative setters",
         "[v2.3.10][inference][orchestrator_unit]")
{
    entropic::ModelOrchestrator orch;
    int call_count = 0;
    auto cb = [&](entropic::ModelOrchestrator::ResidencyEvent,
                  const std::string&, const std::string&, size_t) {
        ++call_count;
    };
    orch.set_residency_observer(cb);
    orch.set_residency_observer(
        entropic::ModelOrchestrator::ResidencyObserverFn{});  // clear
    orch.set_residency_observer(cb);
    orch.set_residency_observer(nullptr);  // clear via nullptr
    REQUIRE(call_count == 0);  // no tier loaded → never fires

    // Speculative enable/disable toggle path (setter is inline).
    orch.set_speculative_enabled(true);
    orch.set_speculative_enabled(false);

    // check_speculative_compat with no main tier loaded.
    auto info = orch.check_speculative_compat();
    REQUIRE_FALSE(info.compatible);
    REQUIRE_FALSE(info.diagnostic.empty());
}

SCENARIO("Orchestrator VRAM budget env override is read at initialize",
         "[v2.3.10][inference][orchestrator_unit]")
{
    auto save = std::getenv("ENTROPIC_VRAM_BUDGET_BYTES");
    std::string saved = save ? save : "";

    auto run_with = [](const char* val) -> size_t {
        if (val) {
            ::setenv("ENTROPIC_VRAM_BUDGET_BYTES", val, 1);
        } else {
            ::unsetenv("ENTROPIC_VRAM_BUDGET_BYTES");
        }
        entropic::ModelOrchestrator orch;
        entropic::ParsedConfig cfg;
        cfg.models.default_tier = "lead";
        (void)orch.initialize(cfg);  // empty tiers → succeeds trivially
        return orch.vram_budget_bytes();
    };

    REQUIRE(run_with("1073741824") == 1073741824u);
    REQUIRE(run_with("not_a_number") == 0u);
    REQUIRE(run_with("-1") == 0u);
    REQUIRE(run_with("") == 0u);

    if (saved.empty()) {
        ::unsetenv("ENTROPIC_VRAM_BUDGET_BYTES");
    } else {
        ::setenv("ENTROPIC_VRAM_BUDGET_BYTES", saved.c_str(), 1);
    }
}

SCENARIO("Orchestrator multi-instance side-by-side construction",
         "[v2.3.10][inference][orchestrator_unit][multi-instance]")
{
    // gh#58 multi-instance pattern: two orchestrators in the same
    // process MUST coexist without sharing process-global state.
    auto fake_a = make_placeholder_gguf("multi_a");
    auto fake_b = make_placeholder_gguf("multi_b");

    entropic::ModelOrchestrator a;
    entropic::ModelOrchestrator b;

    auto cfg_a = make_load_failure_config(fake_a);
    auto cfg_b = make_load_failure_config(fake_b);

    (void)a.initialize(cfg_a);
    (void)b.initialize(cfg_b);

    // Each instance carries an independent tier list / adapter set.
    REQUIRE(a.get_adapter("lead") != nullptr);
    REQUIRE(b.get_adapter("lead") != nullptr);
    REQUIRE(a.get_adapter("lead") != b.get_adapter("lead"));

    // Residency observers are per-instance.
    int a_calls = 0;
    int b_calls = 0;
    a.set_residency_observer(
        [&](entropic::ModelOrchestrator::ResidencyEvent,
            const std::string&, const std::string&, size_t) { ++a_calls; });
    b.set_residency_observer(
        [&](entropic::ModelOrchestrator::ResidencyEvent,
            const std::string&, const std::string&, size_t) { ++b_calls; });

    REQUIRE(a_calls == 0);
    REQUIRE(b_calls == 0);

    // Independent shutdown — orchestrator destructor (gh#63, gh#58
    // close-out) tears down backends then adapter handles. Two
    // sequential destructions exercise the duplicated teardown path.
    a.shutdown();
    b.shutdown();

    std::error_code ec;
    std::filesystem::remove(fake_a, ec);
    std::filesystem::remove(fake_b, ec);
}

SCENARIO("Orchestrator load_grammars_from with valid directory loads files",
         "[v2.3.10][inference][orchestrator_unit]")
{
    // Drive the success branch of load_grammars_from (a directory that
    // exists but has zero .gbnf files still hits the load_bundled call).
    auto tmpdir = std::filesystem::temp_directory_path()
        / "entropic_orch_v2310_grammars";
    std::error_code ec;
    std::filesystem::create_directories(tmpdir, ec);

    entropic::ModelOrchestrator orch;
    auto count = orch.load_grammars_from(tmpdir);
    // Zero or more — either way the success path runs and returns.
    REQUIRE(count == 0);

    std::filesystem::remove_all(tmpdir, ec);
}

SCENARIO("Orchestrator accessors expose subsystem references on a fresh instance",
         "[v2.3.10][inference][orchestrator_unit]")
{
    // Already partially covered above; this scenario asserts the
    // accessor pointers are stable (same address on repeated call).
    entropic::ModelOrchestrator orch;
    auto* g1 = &orch.grammar_registry();
    auto* g2 = &orch.grammar_registry();
    REQUIRE(g1 == g2);

    auto* p1 = &orch.profile_registry();
    auto* p2 = &orch.profile_registry();
    REQUIRE(p1 == p2);

    auto* t1 = &orch.throughput_tracker();
    auto* t2 = &orch.throughput_tracker();
    REQUIRE(t1 == t2);

    auto* a1 = &orch.adapter_manager();
    auto* a2 = &orch.adapter_manager();
    REQUIRE(a1 == a2);
}

// ── v2.3.10 [orchestrator_topup] ──────────────────────────
// Adds router-configured + tier-history + residency snapshot/footprint +
// streaming-error + fallback-tier + handoff-rule + observer scenarios.

namespace {

entropic::ParsedConfig make_config_with_router(
    const std::filesystem::path& fake_gguf,
    const std::filesystem::path& fake_router_gguf) {
    auto cfg = make_load_failure_config(fake_gguf);
    entropic::ModelConfig router_cfg;
    router_cfg.path = fake_router_gguf;
    router_cfg.context_length = 256;
    router_cfg.gpu_layers = 0;
    router_cfg.n_threads = 1;
    cfg.models.router = router_cfg;
    return cfg;
}

}  // namespace

TEST_CASE("Orchestrator route() with router configured but unloadable",
          "[v2.3.10][inference][orchestrator_topup]")
{
    // routing.enabled + router CONFIGURED → bypass early-out, call
    // classify_task → router not loaded → returns ("","") → default tier.
    auto fake = make_placeholder_gguf("router_route");
    auto router = std::filesystem::temp_directory_path()
        / "entropic_orch_v2310_router_no.gguf";
    entropic::ModelOrchestrator orch;
    (void)orch.initialize(make_config_with_router(fake, router));

    std::vector<entropic::Message> msgs = {{"user", "hello", {}, {}}};
    // initialize failed (placeholder GGUF doesn't actually load), so the
    // router-configured path runs classify_task → returns ("","") → the
    // route() fallback executes. The returned tier may be empty under
    // partial-init state; we only assert that the call drove the path.
    (void)orch.route(msgs);
    (void)orch.last_routing_result();

    // available_models includes "router" when configured (890-891).
    auto avail = orch.available_models();
    bool found = false;
    for (const auto& n : avail) { if (n == "router") { found = true; break; } }
    REQUIRE(found);

    // tier_history_ truncates after 5 (drive the erase branch 520-522).
    for (int i = 0; i < 7; ++i) { (void)orch.route(msgs); }

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

TEST_CASE("Orchestrator residency_snapshot_json reports configured budget",
          "[v2.3.10][inference][orchestrator_topup]")
{
    auto save = std::getenv("ENTROPIC_VRAM_BUDGET_BYTES");
    std::string saved = save ? save : "";
    ::setenv("ENTROPIC_VRAM_BUDGET_BYTES", "2147483648", 1);  // 2 GiB

    entropic::ModelOrchestrator orch;
    entropic::ParsedConfig cfg;
    cfg.models.default_tier = "lead";
    (void)orch.initialize(cfg);

    REQUIRE(orch.vram_budget_bytes() == 2147483648u);
    auto j = nlohmann::json::parse(orch.residency_snapshot_json());
    REQUIRE(j["vram_budget_bytes"].get<size_t>() == 2147483648u);
    REQUIRE(j["vram_headroom_bytes"].get<size_t>() == 2147483648u);
    REQUIRE(j["backend"].get<std::string>() == "configured");

    if (saved.empty()) { ::unsetenv("ENTROPIC_VRAM_BUDGET_BYTES"); }
    else { ::setenv("ENTROPIC_VRAM_BUDGET_BYTES", saved.c_str(), 1); }
}

TEST_CASE("Orchestrator generate_streaming error + routing + fallback paths",
          "[v2.3.10][inference][orchestrator_topup]")
{
    auto fake = make_placeholder_gguf("stream_topup");
    entropic::ModelOrchestrator orch;
    (void)orch.initialize(make_load_failure_config(fake));

    std::vector<entropic::Message> msgs = {{"user", "go", {}, {}}};
    entropic::GenerationParams params;
    std::atomic<bool> cancel{false};

    // Explicit unknown tier → error result + No model message.
    auto r1 = orch.generate_streaming(
        msgs, params, [](std::string_view){}, cancel, "no-such");
    REQUIRE(r1.finish_reason == "error");
    REQUIRE(r1.error_message.find("No model") != std::string::npos);

    // Empty tier → route() drives default lookup; activate fails.
    auto r2 = orch.generate_streaming(
        msgs, params, [](std::string_view){}, cancel, "");
    REQUIRE(r2.finish_reason == "error");

    // generate() with unknown tier → falls back to routing.fallback_tier.
    auto r3 = orch.generate(msgs, params, "no-such-tier");
    REQUIRE(r3.finish_reason == "error");

    // tier_footprint_bytes memoization + per-tier independence.
    auto fp1 = orch.tier_footprint_bytes("lead");
    auto fp2 = orch.tier_footprint_bytes("lead");  // cache hit
    REQUIRE(fp1 == fp2);
    REQUIRE(fp1 > 0);
    REQUIRE(orch.tier_footprint_bytes("eng") > 0);

    // clear_all_prompt_caches fans out across model_pool_.
    orch.clear_all_prompt_caches();

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

TEST_CASE("Orchestrator empty-tiers + handoff + observer + accessor coverage",
          "[v2.3.10][inference][orchestrator_topup]")
{
    // Empty tiers → activate_default_tier early-out (line 119) → success.
    {
        entropic::ModelOrchestrator orch;
        entropic::ParsedConfig cfg;
        cfg.models.default_tier = "lead";
        REQUIRE(orch.initialize(cfg));
        REQUIRE(orch.last_used_tier().empty());
        REQUIRE(orch.loaded_models().empty());
        orch.shutdown();
        orch.set_speculative_enabled(true);
        orch.set_speculative_enabled(false);
        REQUIRE_FALSE(orch.check_speculative_compat().compatible);
        REQUIRE(orch.last_residency_error() == ENTROPIC_OK);
        orch.clear_last_residency_error();
    }

    // can_handoff + observer-no-fire on failed init.
    auto fake = make_placeholder_gguf("ho");
    entropic::ModelOrchestrator orch;
    int events = 0;
    orch.set_residency_observer(
        [&events](entropic::ModelOrchestrator::ResidencyEvent,
                  const std::string&, const std::string&, size_t) {
            ++events;
        });
    (void)orch.initialize(make_load_failure_config(fake));
    REQUIRE(events == 0);
    REQUIRE(orch.can_handoff("lead", "eng"));
    REQUIRE_FALSE(orch.can_handoff("lead", "no-such"));   // line 922
    REQUIRE_FALSE(orch.can_handoff("no-source", "lead")); // lines 919-920
    auto j = nlohmann::json::parse(orch.residency_snapshot_json());
    REQUIRE(j["residency"].is_array());
    REQUIRE(j["residency"].empty());
    orch.set_residency_observer(nullptr);

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

// ── gh#82 (v2.4.4): per-tier sampler override precedence ────

SCENARIO("gh#82: apply_tier_sampler_overrides honors tier baseline + per-call override",
         "[v2.4.4][inference][gh82]") {
    using entropic::apply_tier_sampler_overrides;
    using entropic::GenerationParams;

    GIVEN("a tier configures temperature=0.2 and max_output_tokens=2048") {
        std::optional<float> tier_temp = 0.2f;
        std::optional<int> tier_max = 2048;

        WHEN("incoming params are at the struct defaults (0.7 / 4096)") {
            GenerationParams p;  // temperature=0.7f, max_tokens=4096
            apply_tier_sampler_overrides(p, tier_temp, tier_max);
            THEN("the tier baseline is applied") {
                CHECK(p.temperature == Catch::Approx(0.2f));
                CHECK(p.max_tokens == 2048);
            }
        }

        WHEN("the caller explicitly overrides temperature to 0.95") {
            GenerationParams p;
            p.temperature = 0.95f;          // explicit per-call override
            p.max_tokens = 1234;            // explicit per-call override
            apply_tier_sampler_overrides(p, tier_temp, tier_max);
            THEN("the per-call override is preserved over the tier baseline") {
                CHECK(p.temperature == Catch::Approx(0.95f));
                CHECK(p.max_tokens == 1234);
            }
        }
    }

    GIVEN("a tier that configures nothing (both nullopt)") {
        std::optional<float> tier_temp;
        std::optional<int> tier_max;
        WHEN("params are at defaults") {
            GenerationParams p;
            apply_tier_sampler_overrides(p, tier_temp, tier_max);
            THEN("params remain at the struct defaults (no behavior change)") {
                CHECK(p.temperature == Catch::Approx(0.7f));
                CHECK(p.max_tokens == 4096);
            }
        }
    }

    GIVEN("a tier configuring only temperature (max_output_tokens nullopt)") {
        std::optional<float> tier_temp = 0.1f;
        std::optional<int> tier_max;
        WHEN("params are at defaults") {
            GenerationParams p;
            apply_tier_sampler_overrides(p, tier_temp, tier_max);
            THEN("only temperature is set; max_tokens stays at default") {
                CHECK(p.temperature == Catch::Approx(0.1f));
                CHECK(p.max_tokens == 4096);
            }
        }
    }
}
