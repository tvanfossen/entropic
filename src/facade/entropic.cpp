/**
 * @file entropic.cpp
 * @brief librentropic facade — C API implementation.
 *
 * Owns the EntropicEngine handle struct and implements lifecycle
 * (create/configure/destroy), generation, and subsystem stubs.
 * Stubs are wired to real subsystems across Phases 0-5.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/config/loader.h>
#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include "llama_cpp_backend.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

static auto s_log = entropic::log::get("facade");

/**
 * @brief Check handle prerequisites for orchestrator APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_orchestrator(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->configured.load() || !h->orchestrator) {
        return ENTROPIC_ERROR_INVALID_STATE;
    }
    return ENTROPIC_OK;
}

/**
 * @brief Check handle prerequisites for MCP auth APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_mcp_auth(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->configured.load() || !h->mcp_auth) {
        return ENTROPIC_ERROR_INVALID_STATE;
    }
    return ENTROPIC_OK;
}

/**
 * @brief Check handle prerequisites for identity manager APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_identity(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->configured.load() || !h->identity_manager) {
        return ENTROPIC_ERROR_INVALID_STATE;
    }
    return ENTROPIC_OK;
}

/**
 * @brief Find the tier whose config model path matches.
 * @param h Engine handle with orchestrator + config.
 * @param model_path Model file path to match.
 * @return Tier name, or empty string if not found.
 * @internal
 * @version 2.0.0
 */
static std::string find_tier_by_model_path(
    entropic_handle_t h, const std::string& model_path)
{
    for (const auto& [name, tier] : h->config.models.tiers) {
        if (tier.path.string() == model_path) {
            return name;
        }
    }
    return "";
}

/**
 * @brief Resolve tier name to an ACTIVE backend, or throw.
 * @param h Engine handle (must have orchestrator).
 * @param tier_name Tier name string.
 * @return Non-null backend pointer in ACTIVE state.
 * @throws std::runtime_error if tier not found or not active.
 * @internal
 * @version 2.0.0
 */
static entropic::InferenceBackend* require_active_backend(
    entropic_handle_t h, const char* tier_name)
{
    auto* backend = h->orchestrator->get_backend(tier_name);
    if (!backend) {
        throw std::runtime_error(
            "no backend for tier: " + std::string(tier_name));
    }
    if (!backend->is_active()) {
        throw std::runtime_error(
            "model not active: " + std::string(tier_name));
    }
    return backend;
}

extern "C" {

/**
 * @brief Create a new engine instance.
 *
 * Allocates the engine handle struct and initializes Phase 0
 * members (hook_registry, api_mutex, last_error, state flags).
 * Subsystem pointers remain null until entropic_configure().
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_create(entropic_handle_t* handle) {
    if (handle == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    entropic::log::init(spdlog::level::info);
    s_log->info("entropic_create() — v{}", CONFIG_ENTROPIC_VERSION_STRING);

    auto* engine = new (std::nothrow) entropic_engine();
    if (engine == nullptr) {
        *handle = nullptr;
        return ENTROPIC_ERROR_OUT_OF_MEMORY;
    }

    *handle = engine;
    return ENTROPIC_OK;
}

/**
 * @brief Post-parse config setup: load bundled models, set configured.
 *
 * @param h Engine handle.
 * @return ENTROPIC_OK or error code.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t configure_common(entropic_handle_t h) {
    auto data_dir = entropic::config::resolve_data_dir(h->config);
    auto models_yaml = data_dir / "bundled_models.yaml";

    auto err = h->bundled_models.load(models_yaml);
    if (!err.empty()) {
        s_log->warn("bundled models not loaded: {}", err);
    }

    h->orchestrator = std::make_unique<entropic::ModelOrchestrator>();
    if (!h->orchestrator->initialize(h->config)) {
        h->last_error = "orchestrator initialization failed";
        s_log->error("{}", h->last_error);
        return ENTROPIC_ERROR_LOAD_FAILED;
    }

    h->mcp_auth = std::make_unique<entropic::MCPAuthorizationManager>();
    h->identity_manager = std::make_unique<entropic::IdentityManager>(
        entropic::IdentityManagerConfig{});
    h->server_manager = std::make_unique<entropic::ServerManager>(
        h->config.permissions, std::filesystem::current_path());

    h->configured.store(true);
    s_log->info("configure complete — data_dir={}", data_dir.string());
    return ENTROPIC_OK;
}

/**
 * @brief Configure the engine from a JSON/YAML config string.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_configure(
    entropic_handle_t handle,
    const char* config_json) {
    if (!handle || !config_json) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->api_mutex);

    auto err = entropic::config::load_config_from_string(
        config_json, handle->bundled_models, handle->config);
    if (!err.empty()) {
        handle->last_error = err;
        s_log->error("configure: {}", err);
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }

    return configure_common(handle);
}

/**
 * @brief Configure the engine from a YAML config file.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_configure_from_file(
    entropic_handle_t handle,
    const char* config_path) {
    if (!handle || !config_path) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard lock(handle->api_mutex);

    auto err = entropic::config::load_config_from_file(
        config_path, handle->bundled_models, handle->config);
    if (!err.empty()) {
        handle->last_error = err;
        s_log->error("configure_from_file: {}", err);
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }

    return configure_common(handle);
}

/**
 * @brief Destroy an engine instance.
 *
 * Tears down subsystems in reverse creation order. Each
 * subsystem pointer is null-safe. After this call, the
 * handle is invalid.
 *
 * @internal
 * @version 2.0.0
 */
void entropic_destroy(entropic_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    s_log->info("entropic_destroy()");

    // Phase 1+ subsystem teardown will go here in reverse order.
    // Phase 0: struct itself owns hook_registry by value.
    delete handle;
}

/**
 * @brief Get the library version string.
 * @return Static version string.
 * @utility
 * @version 1.8.0
 */
const char* entropic_version(void) {
    return CONFIG_ENTROPIC_VERSION_STRING;
}

/**
 * @brief Get the plugin API version number.
 * @return API version integer.
 * @utility
 * @version 2.0.0
 */
int entropic_api_version(void) {
    return 2;
}

/**
 * @brief Allocate memory using the engine's allocator.
 *
 * @return Pointer to allocated memory, or NULL on failure.
 * @utility
 * @version 1.8.0
 */
void* entropic_alloc(size_t size) {
    return malloc(size);
}

/**
 * @brief Free memory allocated by the engine.
 *
 * @utility
 * @version 1.8.0
 */
void entropic_free(void* ptr) {
    free(ptr);
}

/**
 * @brief Single-turn blocking generation.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_run(
    entropic_handle_t handle,
    const char* input,
    char** result_json) {
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !input || !result_json || !handle->engine) {
        return rc != ENTROPIC_OK ? rc
            : (!input || !result_json) ? ENTROPIC_ERROR_INVALID_ARGUMENT
            : ENTROPIC_ERROR_INVALID_STATE;
    }
    try {
        entropic::Message msg;
        msg.role = "user";
        msg.content = input;
        auto result = handle->engine->run({msg});
        nlohmann::json j = nlohmann::json::array();
        for (const auto& m : result) {
            j.push_back({{"role", m.role}, {"content", m.content}});
        }
        *result_json = strdup(j.dump().c_str());
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("run: {}", handle->last_error);
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
}

/**
 * @brief Streaming generation with token callback.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_run_streaming(
    entropic_handle_t handle,
    const char* input,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag) {
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !input || !on_token) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::atomic<bool> cancel{false};
        auto token_cb = [&](std::string_view tok) {
            on_token(tok.data(), tok.size(), user_data);
            if (cancel_flag && *cancel_flag) { cancel.store(true); }
        };
        entropic::Message msg;
        msg.role = "user";
        msg.content = input;
        auto result = handle->orchestrator->generate_streaming(
            {msg}, entropic::GenerationParams{}, token_cb, cancel);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("run_streaming: {}", handle->last_error);
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
}

/**
 * @brief Interrupt a running generation (thread-safe).
 *
 * @return ENTROPIC_OK if interrupted.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_interrupt(entropic_handle_t handle) {
    if (!handle) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!handle->engine) { return ENTROPIC_ERROR_INVALID_STATE; }
    handle->engine->interrupt();
    return ENTROPIC_OK;
}

// ── LoRA Adapter APIs (v1.9.2 → v2.0.0) ────────────────────

/**
 * @brief Load a LoRA adapter into RAM.
 *
 * Requires a configured engine. The adapter_manager needs llama_model*
 * pointers that are only available from a loaded backend, so this
 * delegates through the orchestrator's backend for the given tier.
 * base_model_path is resolved to a tier via model path matching.
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_adapter_load(
    entropic_handle_t handle,
    const char* adapter_name,
    const char* adapter_path,
    const char* base_model_path,
    float scale)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !adapter_name || !adapter_path || !base_model_path) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto tier = find_tier_by_model_path(handle, base_model_path);
        if (tier.empty()) {
            throw std::runtime_error("no tier for model: "
                + std::string(base_model_path));
        }
        auto* base = handle->orchestrator->get_backend(tier);
        auto* llama = dynamic_cast<entropic::LlamaCppBackend*>(base);
        if (!llama || !llama->llama_model_ptr()) {
            throw std::runtime_error("backend not ready for tier: " + tier);
        }
        bool ok = handle->orchestrator->adapter_manager().load(
            adapter_name, adapter_path, llama->llama_model_ptr(), scale);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_ADAPTER_LOAD_FAILED;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Unload a LoRA adapter.
 *
 * Adapter unload requires llama_context* for deactivation if HOT.
 * The C API cannot safely provide this — use tier-based adapter
 * configuration for lifecycle management.
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_adapter_unload(
    entropic_handle_t handle,
    const char* adapter_name)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !adapter_name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto& mgr = handle->orchestrator->adapter_manager();
        auto info = mgr.info(adapter_name);
        if (info.state == entropic::AdapterState::COLD) {
            throw std::runtime_error("adapter not loaded: "
                + std::string(adapter_name));
        }
        auto tier = handle->orchestrator->last_used_tier();
        auto* base = handle->orchestrator->get_backend(tier);
        auto* llama = dynamic_cast<entropic::LlamaCppBackend*>(base);
        mgr.unload(adapter_name, llama ? llama->llama_context_ptr() : nullptr);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Swap active LoRA adapter.
 *
 * Adapter swap requires llama_context* for activation/deactivation.
 * The C API cannot safely provide this — use tier-based adapter
 * configuration for lifecycle management.
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_adapter_swap(
    entropic_handle_t handle,
    const char* adapter_name)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !adapter_name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto tier = handle->orchestrator->last_used_tier();
        auto* base = handle->orchestrator->get_backend(tier);
        auto* llama = dynamic_cast<entropic::LlamaCppBackend*>(base);
        if (!llama || !llama->llama_context_ptr()) {
            throw std::runtime_error("no active llama context for swap");
        }
        bool ok = handle->orchestrator->adapter_manager().swap(
            adapter_name, llama->llama_context_ptr());
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_ADAPTER_SWAP_FAILED;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Query adapter lifecycle state.
 *
 * Returns the AdapterState as an integer: 0=COLD, 1=WARM, 2=HOT.
 * Returns -1 if the handle is invalid or adapter name not found.
 *
 * @return AdapterState as int, or -1 on error.
 * @internal
 * @version 2.0.0
 */
int entropic_adapter_state(
    entropic_handle_t handle,
    const char* adapter_name)
{
    if (!handle || !handle->configured.load()
        || !handle->orchestrator || !adapter_name) {
        return -1;
    }
    try {
        auto st = handle->orchestrator->adapter_manager().state(adapter_name);
        return static_cast<int>(st);
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("adapter_state: {}", handle->last_error);
        return -1;
    }
}

/**
 * @brief Get adapter info as JSON string.
 *
 * Returns a JSON object with name, state, scale, ram_bytes, path,
 * tier_name, and base_model_path. Caller frees with entropic_free().
 *
 * @return JSON string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_adapter_info(
    entropic_handle_t handle,
    const char* adapter_name)
{
    if (!handle || !handle->configured.load()
        || !handle->orchestrator || !adapter_name) {
        return nullptr;
    }
    try {
        auto ai = handle->orchestrator->adapter_manager().info(adapter_name);
        nlohmann::json j;
        j["name"] = ai.name;
        j["state"] = static_cast<int>(ai.state);
        j["scale"] = ai.scale;
        j["ram_bytes"] = ai.ram_bytes;
        j["path"] = ai.path.string();
        j["tier_name"] = ai.tier_name;
        j["base_model_path"] = ai.base_model_path;
        return strdup(j.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("adapter_info: {}", handle->last_error);
        return nullptr;
    }
}

/**
 * @brief List all known adapters as a JSON array.
 *
 * Returns a JSON array of objects, each with name, state, scale,
 * and tier_name. Caller frees with entropic_free().
 *
 * @return JSON array string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_adapter_list(entropic_handle_t handle)
{
    if (!handle || !handle->configured.load() || !handle->orchestrator) {
        return nullptr;
    }
    try {
        auto adapters = handle->orchestrator->adapter_manager().list_adapters();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : adapters) {
            arr.push_back({{"name", a.name},
                           {"state", static_cast<int>(a.state)},
                           {"scale", a.scale},
                           {"tier_name", a.tier_name}});
        }
        return strdup(arr.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("adapter_list: {}", handle->last_error);
        return nullptr;
    }
}

// ── Grammar Registry APIs (v1.9.3 → v2.0.0) ────────────────

/**
 * @brief Register a grammar by key with GBNF content.
 *
 * Delegates to GrammarRegistry::register_grammar(). The grammar is
 * validated on registration; invalid grammars are still stored with
 * error metadata.
 *
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_ALREADY_EXISTS if
 *         key already registered.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_grammar_register(
    entropic_handle_t handle,
    const char* key,
    const char* gbnf_content)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !key || !gbnf_content) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        bool ok = handle->orchestrator->grammar_registry()
            .register_grammar(key, gbnf_content);
        s_log->info("grammar_register: key={} ok={}", key, ok);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_ALREADY_EXISTS;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("grammar_register: {}", handle->last_error);
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Register a grammar from a GBNF file.
 *
 * Delegates to GrammarRegistry::register_from_file(). File is read,
 * validated, and stored under the given key.
 *
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_IO if file unreadable,
 *         ENTROPIC_ERROR_ALREADY_EXISTS if key exists.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_grammar_register_file(
    entropic_handle_t handle,
    const char* key,
    const char* path)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !key || !path) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        bool ok = handle->orchestrator->grammar_registry()
            .register_from_file(key, path);
        s_log->info("grammar_register_file: key={} ok={}", key, ok);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("grammar_register_file: {}", handle->last_error);
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Remove a grammar from the registry.
 *
 * Delegates to GrammarRegistry::deregister(). Bundled grammars can
 * also be removed (allows overriding defaults).
 *
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_GRAMMAR_NOT_FOUND
 *         if key not registered.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_grammar_deregister(
    entropic_handle_t handle,
    const char* key)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !key) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        bool ok = handle->orchestrator->grammar_registry().deregister(key);
        s_log->info("grammar_deregister: key={} ok={}", key, ok);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_GRAMMAR_NOT_FOUND;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("grammar_deregister: {}", handle->last_error);
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Get grammar GBNF content by key.
 *
 * Returns the raw GBNF content string for the named grammar.
 * Caller frees with entropic_free().
 *
 * @return GBNF string (caller frees), or NULL if not found.
 * @internal
 * @version 2.0.0
 */
char* entropic_grammar_get(
    entropic_handle_t handle,
    const char* key)
{
    if (!handle || !handle->configured.load()
        || !handle->orchestrator || !key) {
        return nullptr;
    }
    try {
        auto content = handle->orchestrator->grammar_registry().get(key);
        return content.empty() ? nullptr : strdup(content.c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("grammar_get: {}", handle->last_error);
        return nullptr;
    }
}

/**
 * @brief Validate a GBNF grammar string without registering.
 *
 * Stateless validation — does not require a handle or loaded model.
 * Delegates to GrammarRegistry::validate().
 *
 * @return NULL if valid; error description string (caller frees) if
 *         invalid.
 * @utility
 * @version 2.0.0
 */
char* entropic_grammar_validate(const char* gbnf_content) {
    if (!gbnf_content) { return strdup("null input"); }
    try {
        auto err = entropic::GrammarRegistry::validate(gbnf_content);
        return err.empty() ? nullptr : strdup(err.c_str());
    } catch (const std::exception& e) {
        return strdup(e.what());
    }
}

/**
 * @brief List all registered grammars as a JSON array.
 *
 * Returns metadata for each grammar: key, source, validated flag,
 * and error string. Content is omitted for efficiency. Caller frees
 * with entropic_free().
 *
 * @return JSON array string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_grammar_list(entropic_handle_t handle)
{
    if (!handle || !handle->configured.load() || !handle->orchestrator) {
        return nullptr;
    }
    try {
        auto entries = handle->orchestrator->grammar_registry().list();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            arr.push_back({{"key", e.key},
                           {"source", e.source},
                           {"validated", e.validated},
                           {"error", e.error}});
        }
        return strdup(arr.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("grammar_list: {}", handle->last_error);
        return nullptr;
    }
}

// ── GPU Resource Profile APIs (v1.9.7 → v2.0.0) ─────────────

/**
 * @brief Register a custom GPU resource profile from JSON.
 *
 * Parses the JSON string into a GPUResourceProfile and delegates to
 * ProfileRegistry::register_profile(). Required JSON fields: "name".
 * Optional: "n_batch", "n_threads", "n_threads_batch", "description".
 *
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_ALREADY_EXISTS if
 *         name exists, ENTROPIC_ERROR_INVALID_ARGUMENT on parse error.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_profile_register(
    entropic_handle_t handle,
    const char* profile_json)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !profile_json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto j = nlohmann::json::parse(profile_json);
        entropic::GPUResourceProfile p;
        p.name = j.value("name", "");
        if (p.name.empty()) { throw std::invalid_argument("missing 'name'"); }
        p.n_batch = j.value("n_batch", 512);
        p.n_threads = j.value("n_threads", 0);
        p.n_threads_batch = j.value("n_threads_batch", 0);
        p.description = j.value("description", "");
        bool ok = handle->orchestrator->profile_registry()
            .register_profile(p);
        s_log->info("profile_register: name={} ok={}", p.name, ok);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_ALREADY_EXISTS;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("profile_register: {}", handle->last_error);
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
}

/**
 * @brief Remove a GPU resource profile by name.
 *
 * Delegates to ProfileRegistry::deregister(). Bundled profiles can
 * be removed.
 *
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_PROFILE_NOT_FOUND
 *         if name not registered.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_profile_deregister(
    entropic_handle_t handle,
    const char* name)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        bool ok = handle->orchestrator->profile_registry().deregister(name);
        s_log->info("profile_deregister: name={} ok={}", name, ok);
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_PROFILE_NOT_FOUND;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("profile_deregister: {}", handle->last_error);
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Get a GPU resource profile by name as JSON.
 *
 * Returns the profile as a JSON object with name, n_batch, n_threads,
 * n_threads_batch, and description. Falls back to "balanced" if name
 * not found (see ProfileRegistry::get). Caller frees with
 * entropic_free().
 *
 * @return JSON string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_profile_get(
    entropic_handle_t handle,
    const char* name)
{
    if (!handle || !handle->configured.load()
        || !handle->orchestrator || !name) {
        return nullptr;
    }
    try {
        auto p = handle->orchestrator->profile_registry().get(name);
        nlohmann::json j;
        j["name"] = p.name;
        j["n_batch"] = p.n_batch;
        j["n_threads"] = p.n_threads;
        j["n_threads_batch"] = p.n_threads_batch;
        j["description"] = p.description;
        return strdup(j.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("profile_get: {}", handle->last_error);
        return nullptr;
    }
}

/**
 * @brief List all registered profile names as a JSON array.
 *
 * Returns a sorted JSON array of profile name strings. Caller frees
 * with entropic_free().
 *
 * @return JSON array string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_profile_list(entropic_handle_t handle)
{
    if (!handle || !handle->configured.load() || !handle->orchestrator) {
        return nullptr;
    }
    try {
        auto names = handle->orchestrator->profile_registry().list();
        nlohmann::json arr = nlohmann::json(names);
        return strdup(arr.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("profile_list: {}", handle->last_error);
        return nullptr;
    }
}

// ── Throughput Query APIs (v1.9.7 → v2.0.0) ─────────────────

/**
 * @brief Get EWMA throughput estimate in tokens per second.
 *
 * Delegates to ThroughputTracker::tok_per_sec(). Returns the
 * smoothed estimate from recent generations. model_path is accepted
 * for API compatibility but the orchestrator tracks a single global
 * throughput (one tracker, not per-model).
 *
 * @return Tokens per second estimate, or 0.0 on error/no data.
 * @internal
 * @version 2.0.0
 */
double entropic_throughput_tok_per_sec(
    entropic_handle_t handle,
    const char* model_path)
{
    (void)model_path;
    if (!handle || !handle->configured.load() || !handle->orchestrator) {
        return 0.0;
    }
    try {
        return handle->orchestrator->throughput_tracker().tok_per_sec();
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("throughput_tok_per_sec: {}", handle->last_error);
        return 0.0;
    }
}

/**
 * @brief Reset throughput tracking data.
 *
 * Clears all recorded samples. model_path is accepted for API
 * compatibility but the orchestrator tracks a single global throughput.
 *
 * @internal
 * @version 2.0.0
 */
void entropic_throughput_reset(
    entropic_handle_t handle,
    const char* model_path)
{
    (void)model_path;
    if (!handle || !handle->configured.load() || !handle->orchestrator) {
        return;
    }
    try {
        handle->orchestrator->throughput_tracker().reset();
        s_log->info("throughput_reset: data cleared");
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("throughput_reset: {}", handle->last_error);
    }
}

// ── MCP Authorization APIs (v1.9.4 → v2.0.0) ────────────────

/**
 * @brief Grant an MCP tool key to an identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_grant_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern,
    entropic_mcp_access_level_t level)
{
    auto rc = check_mcp_auth(handle);
    if (rc != ENTROPIC_OK || !identity_name || !pattern) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    auto lvl = static_cast<entropic::MCPAccessLevel>(level);
    return handle->mcp_auth->grant(identity_name, pattern, lvl);
}

/**
 * @brief Revoke an MCP tool key from an identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_revoke_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern)
{
    auto rc = check_mcp_auth(handle);
    if (rc != ENTROPIC_OK || !identity_name || !pattern) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    return handle->mcp_auth->revoke(identity_name, pattern);
}

/**
 * @brief Check MCP key authorization for an identity.
 *
 * @return 1 if authorized, 0 if denied, -1 on error.
 * @internal
 * @version 2.0.0
 */
int entropic_check_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* tool_name,
    entropic_mcp_access_level_t level)
{
    if (!handle || !handle->configured.load()
        || !handle->mcp_auth || !identity_name || !tool_name) {
        return -1;
    }
    auto lvl = static_cast<entropic::MCPAccessLevel>(level);
    return handle->mcp_auth->check_access(identity_name, tool_name, lvl) ? 1 : 0;
}

/**
 * @brief List MCP keys for an identity as JSON array.
 *
 * @return JSON array string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_list_mcp_keys(
    entropic_handle_t handle,
    const char* identity_name)
{
    if (!handle || !handle->configured.load()
        || !handle->mcp_auth || !identity_name) {
        return nullptr;
    }
    try {
        auto keys = handle->mcp_auth->list_keys(identity_name);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& k : keys) {
            arr.push_back({{"pattern", k.tool_pattern},
                           {"level", static_cast<int>(k.level)}});
        }
        return strdup(arr.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return nullptr;
    }
}

/**
 * @brief Grant a key from one identity to another.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_grant_mcp_key_from(
    entropic_handle_t handle,
    const char* granter,
    const char* grantee,
    const char* pattern,
    entropic_mcp_access_level_t level)
{
    auto rc = check_mcp_auth(handle);
    if (rc != ENTROPIC_OK || !granter || !grantee || !pattern) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    auto lvl = static_cast<entropic::MCPAccessLevel>(level);
    return handle->mcp_auth->grant_from(granter, grantee, pattern, lvl);
}

/**
 * @brief Serialize all identity key sets to JSON.
 *
 * @return JSON string (caller frees), or NULL on error.
 * @internal
 * @version 2.0.0
 */
char* entropic_serialize_mcp_keys(entropic_handle_t handle)
{
    if (!handle || !handle->configured.load() || !handle->mcp_auth) {
        return nullptr;
    }
    try {
        auto json = handle->mcp_auth->serialize_all();
        return strdup(json.c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return nullptr;
    }
}

/**
 * @brief Deserialize all identity key sets from JSON.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_deserialize_mcp_keys(
    entropic_handle_t handle,
    const char* json)
{
    auto rc = check_mcp_auth(handle);
    if (rc != ENTROPIC_OK || !json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    bool ok = handle->mcp_auth->deserialize_all(json);
    return ok ? ENTROPIC_OK : ENTROPIC_ERROR_INVALID_CONFIG;
}

// ── Dynamic Identity Management APIs (v1.9.6 → v2.0.0) ──────

/**
 * @brief Create a dynamic identity from JSON config.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_create_identity(
    entropic_handle_t handle,
    const char* config_json)
{
    auto rc = check_identity(handle);
    if (rc != ENTROPIC_OK || !config_json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto j = nlohmann::json::parse(config_json);
        entropic::IdentityConfig cfg;
        cfg.name = j.value("name", "");
        cfg.system_prompt = j.value("system_prompt", "");
        if (j.contains("focus") && j["focus"].is_array()) {
            cfg.focus = j["focus"].get<std::vector<std::string>>();
        }
        cfg.origin = entropic::IdentityOrigin::DYNAMIC;
        return handle->identity_manager->create(cfg);
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
}

/**
 * @brief Update an existing dynamic identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_update_identity(
    entropic_handle_t handle,
    const char* name,
    const char* config_json)
{
    auto rc = check_identity(handle);
    if (rc != ENTROPIC_OK || !name || !config_json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto j = nlohmann::json::parse(config_json);
        entropic::IdentityConfig cfg;
        cfg.name = name;
        cfg.system_prompt = j.value("system_prompt", "");
        if (j.contains("focus") && j["focus"].is_array()) {
            cfg.focus = j["focus"].get<std::vector<std::string>>();
        }
        cfg.origin = entropic::IdentityOrigin::DYNAMIC;
        return handle->identity_manager->update(name, cfg);
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
}

/**
 * @brief Destroy a dynamic identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_destroy_identity(
    entropic_handle_t handle,
    const char* name)
{
    auto rc = check_identity(handle);
    if (rc != ENTROPIC_OK || !name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    return handle->identity_manager->destroy(name);
}

/**
 * @brief Get identity config as JSON by name.
 *
 * @return JSON string (caller frees), or NULL if not found.
 * @internal
 * @version 2.0.0
 */
char* entropic_get_identity_config(
    entropic_handle_t handle,
    const char* name)
{
    if (!handle || !handle->identity_manager || !name) { return nullptr; }
    try {
        auto* cfg = handle->identity_manager->get(name);
        if (!cfg) { throw std::runtime_error("identity not found"); }
        nlohmann::json j;
        j["name"] = cfg->name;
        j["system_prompt"] = cfg->system_prompt;
        j["origin"] = (cfg->origin == entropic::IdentityOrigin::STATIC)
                       ? "static" : "dynamic";
        return strdup(j.dump().c_str());
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief List all identity names as JSON array.
 *
 * @return JSON array string (caller frees), or NULL.
 * @internal
 * @version 2.0.0
 */
char* entropic_list_identities(entropic_handle_t handle)
{
    if (!handle || !handle->identity_manager) { return nullptr; }
    try {
        auto names = handle->identity_manager->list();
        nlohmann::json arr(names);
        return strdup(arr.dump().c_str());
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief Get identity count (total and dynamic).
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_identity_count(
    entropic_handle_t handle,
    size_t* total,
    size_t* dynamic)
{
    auto rc = check_identity(handle);
    if (rc != ENTROPIC_OK || !total) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    *total = handle->identity_manager->count();
    if (dynamic) { *dynamic = handle->identity_manager->count_dynamic(); }
    return ENTROPIC_OK;
}

// ── Log-Probability Evaluation APIs (v1.9.10 → v2.0.0) ──────

/**
 * @brief Evaluate per-token log-probabilities for a token sequence.
 *
 * Resolves model_id as a tier name, retrieves the backend, and
 * delegates to InferenceBackend::evaluate_logprobs(). The result
 * arrays are engine-allocated — free with entropic_free_logprob_result().
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_get_logprobs(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    entropic_logprob_result_t* result)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !model_id || !tokens || !result || n_tokens < 2) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto* backend = require_active_backend(handle, model_id);
        auto lr = backend->evaluate_logprobs(tokens, n_tokens);
        result->n_tokens = lr.n_tokens;
        result->n_logprobs = lr.n_logprobs;
        result->perplexity = lr.perplexity;
        result->total_logprob = lr.total_logprob;
        result->logprobs = static_cast<float*>(
            malloc(sizeof(float) * lr.logprobs.size()));
        std::copy(lr.logprobs.begin(), lr.logprobs.end(),
                  result->logprobs);
        result->tokens = static_cast<int32_t*>(
            malloc(sizeof(int32_t) * lr.tokens.size()));
        std::copy(lr.tokens.begin(), lr.tokens.end(),
                  result->tokens);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("get_logprobs: {}", handle->last_error);
        return ENTROPIC_ERROR_EVAL_FAILED;
    }
}

/**
 * @brief Compute perplexity for a token sequence.
 *
 * Convenience wrapper — resolves the tier backend and calls
 * InferenceBackend::compute_perplexity().
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_compute_perplexity(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    float* perplexity)
{
    auto rc = check_orchestrator(handle);
    if (rc != ENTROPIC_OK || !model_id || !tokens || !perplexity || n_tokens < 2) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->api_mutex);
        auto* backend = require_active_backend(handle, model_id);
        *perplexity = backend->compute_perplexity(tokens, n_tokens);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("compute_perplexity: {}", handle->last_error);
        return ENTROPIC_ERROR_EVAL_FAILED;
    }
}

/**
 * @brief Free internal arrays of a logprob result.
 *
 * Frees logprobs and tokens arrays, then NULLs the pointers to
 * prevent double-free. The struct itself is caller-owned.
 *
 * @utility
 * @version 1.9.10
 */
void entropic_free_logprob_result(entropic_logprob_result_t* result)
{
    if (result == nullptr) {
        return;
    }
    free(result->logprobs);
    result->logprobs = nullptr;
    free(result->tokens);
    result->tokens = nullptr;
}

// ── Vision Query API (v1.9.11 → v2.0.0) ─────────────────────

/**
 * @brief Check if a model has vision (multimodal) capability.
 *
 * Resolves model_id as a tier name, retrieves the backend, and
 * queries BackendCapability::VISION. Returns 1 if the model has
 * an mmproj loaded, 0 if text-only.
 *
 * @return 1 if vision-capable, 0 if text-only or error.
 * @internal
 * @version 2.0.0
 */
int entropic_model_has_vision(
    entropic_handle_t handle,
    const char* model_id)
{
    if (!handle || !handle->configured.load()
        || !handle->orchestrator || !model_id) {
        return 0;
    }
    try {
        auto* backend = handle->orchestrator->get_backend(model_id);
        return (backend && backend->supports(
            entropic::BackendCapability::VISION)) ? 1 : 0;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        s_log->error("model_has_vision: {}", handle->last_error);
        return 0;
    }
}

// ── Constitutional Validation APIs (v1.9.8 → v2.0.0) ────────

/**
 * @brief Enable or disable constitutional validation globally.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_validation_set_enabled(
    entropic_handle_t handle,
    bool enabled)
{
    if (!handle) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!handle->validator) { return ENTROPIC_ERROR_INVALID_STATE; }
    // Global enable stored in config; validator checks at runtime.
    handle->config.compaction.enabled = enabled;
    return ENTROPIC_OK;
}

/**
 * @brief Set per-identity validation override.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t entropic_validation_set_identity(
    entropic_handle_t handle,
    const char* identity_name,
    bool enabled)
{
    if (!handle || !handle->validator) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_STATE;
    }
    if (!identity_name) { return ENTROPIC_ERROR_INVALID_ARGUMENT; }
    handle->validator->set_identity_validation(identity_name, enabled);
    return ENTROPIC_OK;
}

/**
 * @brief Get last validation result as JSON.
 *
 * @return JSON string (caller frees), or NULL if no result.
 * @internal
 * @version 2.0.0
 */
char* entropic_validation_last_result(entropic_handle_t handle)
{
    if (!handle || !handle->validator) { return nullptr; }
    try {
        auto result = handle->validator->last_result();
        nlohmann::json j;
        j["content"] = result.content;
        j["was_revised"] = result.was_revised;
        j["revision_count"] = result.revision_count;
        return strdup(j.dump().c_str());
    } catch (...) {
        return nullptr;
    }
}

/**
 * @brief Get diagnostic prompt text for /diagnose command (stub).
 *
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 1.9.12
 */
entropic_error_t entropic_get_diagnostic_prompt(
    entropic_handle_t handle,
    char** prompt_out) {
    if (handle == nullptr || prompt_out == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    (void)handle;
    static const char* prompt =
        "[SYSTEM DIRECTIVE: SELF-DIAGNOSIS]\n\n"
        "Analyze your recent actions and identify any issues. "
        "Follow these steps:\n\n"
        "1. Call entropic.diagnose to get a full engine state "
        "snapshot.\n"
        "2. Review the tool call history for:\n"
        "   - Repeated failures (same tool, same error)\n"
        "   - Duplicate tool calls (circuit breaker risk)\n"
        "   - Tool calls that returned errors\n"
        "   - Unexpected state (wrong phase, wrong tier)\n"
        "3. Review your reasoning for:\n"
        "   - Actions that didn't achieve the stated goal\n"
        "   - Unnecessary tool calls\n"
        "   - Missing context that led to errors\n"
        "4. Produce a structured assessment:\n"
        "   - FINDINGS: What went wrong (be specific)\n"
        "   - ROOT CAUSE: Why it went wrong\n"
        "   - RECOMMENDATION: What to do differently\n\n"
        "Be honest and specific. The goal is accurate "
        "self-assessment, not self-defense.\n";
    *prompt_out = strdup(prompt);
    return ENTROPIC_OK;
}

} // extern "C"
