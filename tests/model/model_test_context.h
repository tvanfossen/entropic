/**
 * @file model_test_context.h
 * @brief Shared infrastructure for model tests (subsystem + engine).
 *
 * Extracted from model_tests.cpp to allow subsystem_tests.cpp and
 * engine_tests.cpp to share context, helpers, and C wrappers without
 * duplicating code. All functions are inline (no once-only side
 * effects). g_ctx must only be accessed inside SCENARIO blocks.
 *
 * @version 1.10.2
 */

#pragma once

#include <entropic/config/bundled_models.h>
#include <entropic/config/identity.h>
#include <entropic/config/loader.h>
#include <entropic/core/identity_manager.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/prompts/manager.h>
#include <entropic/interfaces/i_inference_callbacks.h>
#include <entropic/mcp/mcp_authorization.h>
#include <entropic/types/backend_capability.h>
#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/logprob_result.h>
#include <entropic/types/message.h>
#include <entropic/entropic_config.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace entropic;

// ── Shared test context ─────────────────────────────────────

/**
 * @brief Shared model test context — loaded once, reused across tests.
 * @version 1.10.2
 */
struct ModelTestContext {
    std::unique_ptr<ModelOrchestrator> orchestrator; ///< Model orchestrator
    config::BundledModels registry;                  ///< Bundled model registry
    ParsedConfig config;                             ///< Parsed configuration
    std::string model_path;                          ///< Resolved model path
    std::string default_tier;                        ///< Default tier name
    bool initialized = false;                        ///< Init success flag
};

/// @brief Global test context singleton.
/// @internal
/// @version 1.10.2
inline ModelTestContext g_ctx;

// ── Test parameters ─────────────────────────────────────────

/// @brief Default generation params for model tests.
/// @version 1.10.2
constexpr float TEST_TEMPERATURE = 0.0f;
/// @brief Max tokens for generation (matches lead identity default).
/// @version 1.10.2
constexpr int TEST_MAX_TOKENS = 1024;
/// @brief Max retries per test before FAIL.
/// @version 1.10.2
constexpr int MAX_RETRIES = 2;

// ── Config loading helpers ──────────────────────────────────

/**
 * @brief Resolve bundled_models.yaml path from project source.
 * @return Path to bundled_models.yaml.
 * @utility
 * @version 1.10.2
 */
inline fs::path bundled_models_path() {
    fs::path data_dir = fs::path(MODEL_PATH) / "data";
    if (fs::exists(data_dir / "bundled_models.yaml")) {
        return data_dir / "bundled_models.yaml";
    }
    return fs::path(MODEL_PATH)
        / "python" / "entropic" / "data" / "bundled_models.yaml";
}

/**
 * @brief Load bundled models registry from YAML.
 * @param registry Output registry.
 * @return true on success.
 * @utility
 * @version 1.10.2
 */
inline bool load_registry(config::BundledModels& registry) {
    auto path = bundled_models_path();
    auto err = registry.load(path);
    if (!err.empty()) {
        spdlog::error("Failed to load bundled_models.yaml: {}", err);
        return false;
    }
    return true;
}

/**
 * @brief Load test config using standard layered resolution.
 * @param registry Bundled model registry.
 * @param config Output parsed config.
 * @return true on success.
 * @utility
 * @version 1.10.2
 */
inline bool load_test_config(const config::BundledModels& registry,
                             ParsedConfig& config) {
    fs::path global = fs::path(getenv("HOME")) / ".entropic" / "config.yaml";
    fs::path project = ".entropic/config.local.yaml";
    auto err = config::load_config(global, project, registry, config);
    if (!err.empty()) {
        spdlog::error("Config load failed: {}", err);
        return false;
    }
    return true;
}

/**
 * @brief Initialize the orchestrator with loaded config.
 * @param ctx Test context to populate.
 * @return true on success.
 * @utility
 * @version 1.10.2
 */
inline bool init_orchestrator(ModelTestContext& ctx) {
    ctx.orchestrator = std::make_unique<ModelOrchestrator>();
    if (!ctx.orchestrator->initialize(ctx.config)) {
        spdlog::error("Orchestrator init failed");
        return false;
    }
    ctx.default_tier = ctx.config.models.default_tier;
    ctx.initialized = true;
    return true;
}

// ── Prompt loading helpers ──────────────────────────────────

/**
 * @brief Resolve path to the bundled prompts directory.
 * @return Path to python/entropic/data/prompts/.
 * @utility
 * @version 1.10.2
 */
inline fs::path bundled_prompts_dir() {
    return fs::path(MODEL_PATH) / "python" / "entropic" / "data" / "prompts";
}

/**
 * @brief Load a bundled identity's system prompt body.
 * @param tier_name Tier name (e.g., "lead").
 * @return System prompt body, or empty string on failure.
 * @utility
 * @version 1.10.2
 */
inline std::string load_identity_prompt(const std::string& tier_name) {
    auto path = bundled_prompts_dir()
                / ("identity_" + tier_name + ".md");
    entropic::prompts::ParsedIdentity identity;
    auto err = entropic::prompts::load_identity(path, identity);
    if (!err.empty()) {
        spdlog::error("Failed to load identity '{}': {}", tier_name, err);
        return "";
    }
    return identity.body;
}

/**
 * @brief Load the bundled constitution text.
 * @return Constitution body, or empty string on failure.
 * @utility
 * @version 1.10.2
 */
inline std::string load_constitution_prompt() {
    std::string body;
    auto data_dir = fs::path(MODEL_PATH) / "python" / "entropic" / "data";
    auto err = entropic::prompts::load_constitution(
        std::nullopt, false, data_dir, body);
    if (!err.empty()) {
        spdlog::error("Failed to load constitution: {}", err);
        return "";
    }
    return body;
}

/**
 * @brief Load the bundled app_context text.
 * @return App context body, or empty string on failure.
 * @utility
 * @version 1.10.2
 */
inline std::string load_app_context_prompt() {
    std::string body;
    auto data_dir = fs::path(MODEL_PATH) / "python" / "entropic" / "data";
    auto path = data_dir / "prompts" / "app_context.md";
    auto err = entropic::prompts::load_app_context(
        path, false, data_dir, body);
    if (!err.empty()) {
        spdlog::error("Failed to load app_context: {}", err);
        return "";
    }
    return body;
}

/**
 * @brief Assemble the full system prompt: constitution + app_context + identity.
 * @param tier_name Tier name (e.g., "lead").
 * @return Assembled system prompt, or empty string on failure.
 * @utility
 * @version 1.10.2
 */
inline std::string assemble_system_prompt(const std::string& tier_name) {
    auto constitution = load_constitution_prompt();
    auto app_context = load_app_context_prompt();
    auto identity = load_identity_prompt(tier_name);
    if (identity.empty()) { return ""; }

    std::string prompt;
    if (!constitution.empty()) {
        prompt += constitution + "\n\n";
    }
    if (!app_context.empty()) {
        prompt += app_context + "\n\n";
    }
    prompt += identity;
    return prompt;
}

// ── Message helpers ─────────────────────────────────────────

/**
 * @brief Build a system + user message pair.
 * @param system_text System prompt.
 * @param user_text User message.
 * @return Message vector.
 * @utility
 * @version 1.10.2
 */
inline std::vector<Message> make_messages(const std::string& system_text,
                                          const std::string& user_text) {
    Message sys;
    sys.role = "system";
    sys.content = system_text;
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

/**
 * @brief Build default generation params for model tests.
 * @return GenerationParams with temperature=0, seed=42.
 * @utility
 * @version 1.10.2
 */
inline GenerationParams test_gen_params() {
    GenerationParams p;
    p.temperature = TEST_TEMPERATURE;
    p.max_tokens = TEST_MAX_TOKENS;
    return p;
}

// ── JSON helpers for InferenceInterface wrappers ────────────

/**
 * @brief Deserialize JSON message array to vector<Message>.
 * @param msgs_json JSON array string.
 * @return Deserialized messages.
 * @utility
 * @version 1.10.2
 */
inline std::vector<Message> parse_messages_json(const char* msgs_json) {
    std::vector<Message> messages;
    auto arr = json::parse(msgs_json, nullptr, false);
    if (!arr.is_array()) { return messages; }
    for (const auto& obj : arr) {
        Message m;
        m.role = obj.value("role", "");
        m.content = obj.value("content", "");
        messages.push_back(std::move(m));
    }
    return messages;
}

/**
 * @brief Parse generation params from JSON, with test defaults.
 * @param params_json JSON params string from engine.
 * @return GenerationParams with engine values or test defaults.
 * @utility
 * @version 1.10.2
 */
inline GenerationParams parse_gen_params(const char* params_json) {
    auto params = test_gen_params();
    if (params_json == nullptr) { return params; }
    auto obj = json::parse(params_json, nullptr, false);
    if (!obj.is_object()) { return params; }
    if (obj.contains("max_tokens")) {
        params.max_tokens = obj["max_tokens"].get<int>();
    }
    if (obj.contains("temperature")) {
        params.temperature = obj["temperature"].get<float>();
    }
    return params;
}

/**
 * @brief Allocate a C string copy (freed by real_free).
 * @param s Source string.
 * @return Heap-allocated copy.
 * @utility
 * @version 1.10.2
 */
inline char* alloc_cstr(const std::string& s) {
    auto* p = static_cast<char*>(malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// ── InferenceInterface C wrappers ───────────────────────────
//
// These thin wrappers bridge the C function pointer interface
// expected by AgentEngine to the real C++ orchestrator/adapter.
// user_data points to ModelTestContext (g_ctx).

/**
 * @brief Batch generate via orchestrator (C wrapper).
 * @param msgs_json JSON messages (borrowed).
 * @param params_json Generation params JSON (parsed for overrides).
 * @param result_json Output response string.
 * @param user_data ModelTestContext pointer.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
inline int real_generate(const char* msgs_json,
                         const char* params_json,
                         char** result_json,
                         void* user_data) {
    auto* ctx = static_cast<ModelTestContext*>(user_data);
    auto messages = parse_messages_json(msgs_json);
    auto params = parse_gen_params(params_json);
    auto result = ctx->orchestrator->generate(
        messages, params, ctx->default_tier);
    *result_json = alloc_cstr(result.raw_content.empty()
        ? result.content : result.raw_content);
    return 0;
}

/**
 * @brief Streaming generate via orchestrator (C wrapper).
 * @param msgs_json JSON messages.
 * @param params_json Generation params JSON (parsed for overrides).
 * @param on_token Token callback.
 * @param token_ud Token callback user data.
 * @param cancel Cancel flag pointer.
 * @param user_data ModelTestContext pointer.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
inline int real_generate_stream(const char* msgs_json,
                                const char* params_json,
                                void (*on_token)(const char*, size_t, void*),
                                void* token_ud,
                                int* cancel,
                                void* user_data)
{
    if (cancel != nullptr && *cancel != 0) { return 0; }
    auto* ctx = static_cast<ModelTestContext*>(user_data);
    auto messages = parse_messages_json(msgs_json);
    auto params = parse_gen_params(params_json);
    std::atomic<bool> cancel_flag(false);
    std::function<void(std::string_view)> stream_cb =
        [on_token, token_ud](std::string_view tok)
        { on_token(tok.data(), tok.size(), token_ud); };
    ctx->orchestrator->generate_streaming(
        messages, params, stream_cb, cancel_flag, ctx->default_tier);
    return 0;
}

/**
 * @brief Route messages to tier (C wrapper).
 * @param msgs_json JSON messages.
 * @param result_json Output tier name.
 * @param user_data ModelTestContext pointer.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
inline int real_route(const char* msgs_json,
                      char** result_json,
                      void* user_data) {
    auto* ctx = static_cast<ModelTestContext*>(user_data);
    auto messages = parse_messages_json(msgs_json);
    auto tier = ctx->orchestrator->route(messages);
    *result_json = alloc_cstr(tier);
    return 0;
}

/**
 * @brief Raw text completion (C wrapper).
 * @param prompt Raw prompt string.
 * @param params_json Params (unused).
 * @param result_json Output response string.
 * @param user_data ModelTestContext pointer.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
inline int real_complete(const char* prompt,
                         const char* /*params_json*/,
                         char** result_json,
                         void* user_data) {
    auto* ctx = static_cast<ModelTestContext*>(user_data);
    auto messages = make_messages("", prompt);
    auto params = test_gen_params();
    params.max_tokens = 1;
    auto result = ctx->orchestrator->generate(
        messages, params, ctx->default_tier);
    *result_json = alloc_cstr(result.content);
    return 0;
}

/**
 * @brief Parse tool calls from raw content (C wrapper).
 * @param raw_content Raw model output.
 * @param cleaned_content Output cleaned content.
 * @param tool_calls_json Output tool calls JSON.
 * @param user_data ModelTestContext pointer.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
/**
 * @brief Serialize parsed tool calls to JSON array string.
 * @param parsed Parsed tool call result from adapter.
 * @return JSON array as string.
 * @utility
 * @version 2.0.0
 */
inline std::string serialize_tool_calls(
    const entropic::ParseResult& parsed)
{
    json tools = json::array();
    for (const auto& tc : parsed.tool_calls) {
        json args_obj;
        for (const auto& [k, v] : tc.arguments) { args_obj[k] = v; }
        tools.push_back({{"name", tc.name}, {"arguments", args_obj}});
    }
    return tools.dump();
}

/**
 * @brief Parse tool calls from raw model output via the adapter.
 * @param raw_content Raw model output string.
 * @param cleaned_content Output: content with tool calls stripped.
 * @param tool_calls_json Output: JSON array of parsed tool calls.
 * @param user_data Pointer to ModelTestContext.
 * @return 0 on success, non-zero on failure.
 * @internal
 * @version 2.0.0
 */
inline int real_parse_tool_calls(const char* raw_content,
                                 char** cleaned_content,
                                 char** tool_calls_json,
                                 void* user_data)
{
    auto* ctx = static_cast<ModelTestContext*>(user_data);
    auto* adapter = ctx->orchestrator->get_adapter(ctx->default_tier);
    if (adapter == nullptr) {
        *cleaned_content = alloc_cstr(raw_content);
        *tool_calls_json = alloc_cstr("[]");
        return 0;
    }
    auto parsed = adapter->parse_tool_calls(raw_content);
    *cleaned_content = alloc_cstr(parsed.cleaned_content);
    *tool_calls_json = alloc_cstr(serialize_tool_calls(parsed));
    return 0;
}

/**
 * @brief Check if response is complete (C wrapper).
 * @param content Response content.
 * @param tool_calls_json Tool calls JSON.
 * @param user_data Unused.
 * @return 1 if complete, 0 if not.
 * @callback
 * @version 1.10.2
 */
inline int real_is_complete(const char* /*content*/,
                            const char* tool_calls_json,
                            void* /*user_data*/) {
    auto tc = json::parse(tool_calls_json, nullptr, false);
    if (tc.is_array() && !tc.empty()) { return 0; }
    return 1;
}

/**
 * @brief Free a C string allocated by wrappers.
 * @param ptr Pointer to free.
 * @callback
 * @version 1.10.2
 */
inline void real_free(void* ptr) {
    free(ptr);
}

/**
 * @brief Build an InferenceInterface wired to the live model.
 * @return Wired interface.
 * @utility
 * @version 1.10.2
 */
inline InferenceInterface make_real_interface() {
    InferenceInterface iface;
    iface.generate = real_generate;
    iface.generate_stream = real_generate_stream;
    iface.route = real_route;
    iface.complete = real_complete;
    iface.parse_tool_calls = real_parse_tool_calls;
    iface.is_response_complete = real_is_complete;
    iface.free_fn = real_free;
    iface.backend_data = &g_ctx;
    iface.orchestrator_data = &g_ctx;
    iface.adapter_data = &g_ctx;
    return iface;
}

// ── VRAM query helper ───────────────────────────────────────

/**
 * @brief Query current VRAM usage from the loaded backend.
 * @return VRAM bytes, or 0 if unavailable.
 * @utility
 * @version 1.10.2
 */
inline size_t query_vram_bytes() {
    if (!g_ctx.orchestrator) { return 0; }
    auto* backend = g_ctx.orchestrator->get_backend(g_ctx.default_tier);
    if (!backend) { return 0; }
    return backend->info().vram_bytes;
}

// ── Per-test log sink ────────────────────────────────────────
//
// Each test gets its own file sink. Before a test starts, we replace
// the sinks on every registered logger with [console + this test's file].
// After the test, we remove the file sink. No accumulation, no leaks.

/// @brief Log directory for model test output.
/// @internal
/// @version 1.10.2
inline const fs::path LOG_DIR = fs::path(TEST_REPORTS_DIR) / "logs";

/// @brief Console sink shared across all tests (created once).
/// @internal
/// @version 1.10.2
inline std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> g_console_sink;

/// @brief Current test's file sink (replaced per test).
/// @internal
/// @version 1.10.2
inline std::shared_ptr<spdlog::sinks::basic_file_sink_mt> g_file_sink;

/**
 * @brief Set sinks on all registered loggers to the given list.
 * @param sinks Sink list to install.
 * @utility
 * @version 1.10.2
 */
inline void set_all_sinks(
    const std::vector<spdlog::sink_ptr>& sinks) {
    spdlog::apply_all([&sinks](std::shared_ptr<spdlog::logger> l) {
        l->sinks() = sinks;
        l->set_level(spdlog::level::trace);
    });
}

/**
 * @brief Start logging for a test — new file, clean sinks.
 * @param test_name Filesystem-safe test name.
 * @utility
 * @version 1.10.2
 */
inline void start_test_log(const std::string& test_name) {
    fs::create_directories(LOG_DIR);
    if (!g_console_sink) {
        g_console_sink =
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }
    g_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (LOG_DIR / (test_name + ".log")).string(), true);
    g_file_sink->set_level(spdlog::level::trace);
    set_all_sinks({g_console_sink, g_file_sink});
    spdlog::info("=== {} ===", test_name);
}

/**
 * @brief Stop file logging after a test — console only.
 * @utility
 * @version 1.10.2
 */
inline void end_test_log() {
    if (!g_file_sink) { return; }
    size_t vram = query_vram_bytes();
    spdlog::info("VRAM at test end: {} bytes ({} MB)",
                 vram, vram / (1024 * 1024));
    g_file_sink->flush();
    set_all_sinks({g_console_sink});
    g_file_sink.reset();
}

// ── Catch2 event listener for model lifecycle ───────────────

/// @brief VRAM monitoring threshold (64 MB).
/// @version 1.10.2
constexpr size_t VRAM_LEAK_THRESHOLD = 64 * 1024 * 1024;

/**
 * @brief Catch2 listener — model loading, VRAM monitoring.
 * @version 1.10.2
 */
class ModelTestListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    /**
     * @brief Load model before test run.
     * @param info Test run info.
     * @utility
     * @version 1.10.2
     */
    void testRunStarting(Catch::TestRunInfo const& /*info*/) override {
        spdlog::info("Loading model for model tests...");
        fs::create_directories(LOG_DIR);
        bool ok = load_registry(g_ctx.registry);
        ok = ok && load_test_config(g_ctx.registry, g_ctx.config);
        ok = ok && init_orchestrator(g_ctx);
        if (!ok) {
            spdlog::error("Model test init failed — tests will skip");
        }
    }

    /**
     * @brief Snapshot VRAM before test.
     * @param info Test case info.
     * @utility
     * @version 1.10.2
     */
    void testCaseStarting(Catch::TestCaseInfo const& /*tc*/) override {
        vram_before_ = query_vram_bytes();
    }

    /**
     * @brief Check VRAM delta after test.
     * @param stats Test case stats.
     * @utility
     * @version 1.10.2
     */
    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        size_t vram_after = query_vram_bytes();
        if (vram_before_ > 0 && vram_after > vram_before_) {
            size_t delta = vram_after - vram_before_;
            if (delta > VRAM_LEAK_THRESHOLD) {
                spdlog::warn("VRAM leak: {} -> {} ({} MB delta) in {}",
                    vram_before_, vram_after,
                    delta / (1024 * 1024),
                    stats.testInfo->name);
            }
        }
        spdlog::info("VRAM: before={} after={} ({})",
            vram_before_, vram_after,
            stats.testInfo->name);
    }

    /**
     * @brief Unload model after run.
     * @param stats Test run stats.
     * @utility
     * @version 1.10.2
     */
    void testRunEnded(Catch::TestRunStats const& /*stats*/) override {
        if (g_ctx.orchestrator) {
            g_ctx.orchestrator->shutdown();
        }
        spdlog::info("Model tests complete — logs in {}", LOG_DIR.string());
    }

private:
    size_t vram_before_ = 0; ///< VRAM snapshot before test case
};
