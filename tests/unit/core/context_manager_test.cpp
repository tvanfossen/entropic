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

TEST_CASE("prune_old_tool_results TTL-based",
          "[context_manager]") {
    TokenCounter tc(10000);
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
