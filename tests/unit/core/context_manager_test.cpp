// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_context_manager.cpp
 * @brief ContextManager unit tests.
 * @version 1.8.4
 */

#include <entropic/core/context_manager.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Create a message with role and content.
 * @param role Message role.
 * @param content Message content.
 * @return Message struct.
 * @internal
 * @version 1.8.4
 */
static Message make_msg(const std::string& role,
                        const std::string& content) {
    Message m;
    m.role = role;
    m.content = content;
    return m;
}

/**
 * @brief Create a tool result message.
 * @param name Tool name.
 * @param content Result content.
 * @param iter Added-at iteration.
 * @return Message with metadata.
 * @internal
 * @version 1.8.4
 */
static Message make_tool_msg(const std::string& name,
                             const std::string& content,
                             int iter) {
    Message m;
    m.role = "user";
    m.content = content;
    m.metadata["tool_name"] = name;
    m.metadata["added_at_iteration"] = std::to_string(iter);
    return m;
}

// ── Tests ────────────────────────────────────────────────

TEST_CASE("prune_tool_results stubs old results",
          "[context_manager]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.messages.push_back(make_msg("system", "sys"));
    ctx.messages.push_back(make_tool_msg("fs.read", "long result", 1));
    ctx.messages.push_back(make_tool_msg("fs.write", "ok", 2));
    ctx.messages.push_back(make_tool_msg("bash.run", "output", 3));

    auto [pruned, freed] = ctxm.prune_tool_results(ctx, 1);
    REQUIRE(pruned == 2);
    REQUIRE(freed > 0);
    // First tool result should be stubbed
    REQUIRE(ctx.messages[1].content.find("[Previous:") == 0);
    // Last tool result should be kept
    REQUIRE(ctx.messages[3].content == "output");
}

TEST_CASE("prune_tool_results keeps recent count",
          "[context_manager]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.messages.push_back(make_tool_msg("a", "aaa", 1));
    ctx.messages.push_back(make_tool_msg("b", "bbb", 2));
    ctx.messages.push_back(make_tool_msg("c", "ccc", 3));

    auto [pruned, freed] = ctxm.prune_tool_results(ctx, 2);
    REQUIRE(pruned == 1);
    REQUIRE(ctx.messages[0].content.find("[Previous:") == 0);
    REQUIRE(ctx.messages[1].content == "bbb");
    REQUIRE(ctx.messages[2].content == "ccc");
}

TEST_CASE("prune_old_tool_results TTL-based at high context fill",
          "[context_manager]") {
    // v2.1.3 (#6): prune is now gated on context fill — under
    // warning_threshold_percent (default 0.6) it's a no-op. This test
    // uses a tiny token budget so the trivial messages cross the
    // threshold and TTL pruning engages, preserving the original
    // assertion that TTL-based selection still works once the gate
    // releases.
    TokenCounter tc(20);  // tiny budget — small messages cross threshold
    CompactionConfig cfg;
    cfg.tool_result_ttl = 3;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 5;
    ctx.messages.push_back(make_tool_msg("old", "old data", 1));
    ctx.messages.push_back(make_tool_msg("new", "new data", 4));

    ctxm.prune_old_tool_results(ctx);
    REQUIRE(ctx.messages[0].content.find("[Previous:") == 0);
    REQUIRE(ctx.messages[1].content == "new data");
}

TEST_CASE("prune_old_tool_results is a no-op below warning threshold",
          "[context_manager][regression][2.1.3][issue-6]") {
    // Issue #6 regression: pre-2.1.3 prune fired on every iteration
    // regardless of context fill. A 25-min session against a 32K
    // context window observed 78 prune events at peak fill 29%, well
    // below the warning threshold. This test pins the gate: with
    // small messages and a generous budget, fill sits well below the
    // threshold and prune must NOT touch tool results, even when
    // their age exceeds TTL.
    TokenCounter tc(100000);  // huge budget — fill stays near zero
    CompactionConfig cfg;
    cfg.tool_result_ttl = 3;
    cfg.warning_threshold_percent = 0.6f;  // default
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 100;  // FAR past TTL — pre-fix would prune
    ctx.messages.push_back(make_tool_msg("ancient", "result A", 1));
    ctx.messages.push_back(make_tool_msg("recent", "result B", 99));

    ctxm.prune_old_tool_results(ctx);
    REQUIRE(ctx.messages[0].content == "result A");  // NOT pruned
    REQUIRE(ctx.messages[1].content == "result B");
}

TEST_CASE("prune_old_tool_results engages once fill crosses threshold",
          "[context_manager][regression][2.1.3][issue-6]") {
    // Companion to the no-op test: once context fill rises above
    // warning_threshold_percent, TTL pruning kicks in. Forces fill
    // by setting a tiny budget so even short messages cross the
    // threshold.
    TokenCounter tc(10);  // tiny budget — fill exceeds 60% trivially
    CompactionConfig cfg;
    cfg.tool_result_ttl = 3;
    cfg.warning_threshold_percent = 0.6f;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 100;
    ctx.messages.push_back(make_tool_msg("ancient", "result A", 1));
    ctx.messages.push_back(make_tool_msg("recent", "result B", 99));

    ctxm.prune_old_tool_results(ctx);
    REQUIRE(ctx.messages[0].content.find("[Previous:") == 0);  // pruned
    REQUIRE(ctx.messages[1].content == "result B");           // kept
}

TEST_CASE("prune_old_tool_results preserves original_content in metadata "
          "for validator evidence (#5)",
          "[context_manager][regression][2.1.3][issue-5]") {
    // Issue #5: validator runs against post-prune messages and
    // false-flags valid file:line citations as hallucinated because
    // the prune stub is the only evidence visible to it. The fix
    // preserves the un-pruned content in ``msg.metadata
    // ["original_content"]`` so the engine can surface it via the
    // POST_GENERATE hook's tool_evidence field.
    TokenCounter tc(10);  // tiny budget — forces prune to engage
    CompactionConfig cfg;
    cfg.tool_result_ttl = 1;
    cfg.warning_threshold_percent = 0.6f;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 10;
    const std::string original = "src/foo.cpp:42 — load_config returns ENTROPIC_OK";
    ctx.messages.push_back(make_tool_msg("fs.read", original, 1));

    ctxm.prune_old_tool_results(ctx);

    // Content is replaced with the stub (model adapter still saves
    // budget on next inference)
    REQUIRE(ctx.messages[0].content.find("[Previous:") == 0);
    REQUIRE(ctx.messages[0].content.find(original) == std::string::npos);

    // But original survives in metadata for the validator
    auto& meta = ctx.messages[0].metadata;
    REQUIRE(meta.find("original_content") != meta.end());
    REQUIRE(meta["original_content"] == original);
}

TEST_CASE("prune_old_tool_results respects warning_threshold_percent=0 "
          "(restores pre-2.1.3 always-prune)",
          "[context_manager][regression][2.1.3][issue-6]") {
    // Operators who relied on the pre-2.1.3 always-prune behaviour
    // (e.g. tight VRAM budgets where every char of context matters
    // regardless of fill) can restore it by setting
    // warning_threshold_percent to 0. Documented escape hatch.
    TokenCounter tc(100000);  // would normally suppress prune
    CompactionConfig cfg;
    cfg.tool_result_ttl = 3;
    cfg.warning_threshold_percent = 0.0f;  // ALWAYS prune
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 5;
    ctx.messages.push_back(make_tool_msg("old", "old data", 1));

    ctxm.prune_old_tool_results(ctx);
    REQUIRE(ctx.messages[0].content.find("[Previous:") == 0);
}

TEST_CASE("Context warning injected at threshold",
          "[context_manager]") {
    TokenCounter tc(100);
    CompactionConfig cfg;
    cfg.warning_threshold_percent = 0.3f;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 1;
    // Fill enough to exceed 30% of 100 tokens
    ctx.messages.push_back(make_msg("user", std::string(200, 'x')));

    ctxm.inject_context_warning(ctx);
    REQUIRE(ctx.messages.size() == 2);
    REQUIRE(ctx.messages[1].content.find("[CONTEXT WARNING]") == 0);
}

TEST_CASE("Context warning not repeated same iteration",
          "[context_manager]") {
    TokenCounter tc(100);
    CompactionConfig cfg;
    cfg.warning_threshold_percent = 0.3f;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctx.metrics.iterations = 1;
    ctx.messages.push_back(make_msg("user", std::string(200, 'x')));

    ctxm.inject_context_warning(ctx);
    size_t after_first = ctx.messages.size();
    ctxm.inject_context_warning(ctx);
    REQUIRE(ctx.messages.size() == after_first);
}

TEST_CASE("refresh_context_limit updates counter",
          "[context_manager]") {
    TokenCounter tc(1000);
    CompactionConfig cfg;
    EngineCallbacks cb;
    CompactionManager cm(cfg, tc);
    ContextManager ctxm(cm, cb);

    LoopContext ctx;
    ctxm.refresh_context_limit(ctx, 2000);
    REQUIRE(tc.max_tokens == 2000);
}
