/**
 * @file entropic.cpp
 * @brief librentropic facade — lifecycle stubs for v1.8.0.
 *
 * Real implementations land in subsequent versions:
 * - entropic_create/configure/destroy: v1.8.4 (engine loop)
 * - entropic_run/run_streaming: v1.8.4 (engine loop)
 * - entropic_interrupt: v1.8.4 (engine loop)
 *
 * @version 1.8.0
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include <cstdlib>
#include <cstring>

static auto s_log = entropic::log::get("facade");

extern "C" {

/**
 * @brief Create a new engine instance (stub).
 * @param handle Pointer to receive the new handle.
 * @return ENTROPIC_ERROR_INTERNAL — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_create(entropic_handle_t* handle) {
    if (handle == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    entropic::log::init(spdlog::level::info);
    s_log->info("entropic_create() — v{}", CONFIG_ENTROPIC_VERSION_STRING);
    *handle = nullptr;
    return ENTROPIC_ERROR_INTERNAL;
}

/**
 * @brief Configure the engine from JSON (stub).
 * @param handle Engine handle.
 * @param config_json JSON config string.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_configure(
    entropic_handle_t handle,
    const char* config_json) {
    (void)handle;
    (void)config_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Configure engine from file (stub).
 * @param handle Engine handle.
 * @param config_path Path to config file.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_configure_from_file(
    entropic_handle_t handle,
    const char* config_path) {
    (void)handle;
    (void)config_path;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Destroy an engine instance (stub).
 * @param handle Engine handle. NULL is a no-op.
 * @internal
 * @version 1.8.0
 */
void entropic_destroy(entropic_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
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
 * @version 1.8.0
 */
int entropic_api_version(void) {
    return 1;
}

/**
 * @brief Allocate memory using the engine's allocator.
 * @param size Number of bytes.
 * @return Pointer to allocated memory, or NULL on failure.
 * @utility
 * @version 1.8.0
 */
void* entropic_alloc(size_t size) {
    return malloc(size);
}

/**
 * @brief Free memory allocated by the engine.
 * @param ptr Pointer to free. NULL is a no-op.
 * @utility
 * @version 1.8.0
 */
void entropic_free(void* ptr) {
    free(ptr);
}

/**
 * @brief Single-turn blocking generation (stub).
 * @param handle Engine handle.
 * @param input User message.
 * @param result_json Output JSON result.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_run(
    entropic_handle_t handle,
    const char* input,
    char** result_json) {
    (void)handle;
    (void)input;
    (void)result_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Streaming generation (stub).
 * @param handle Engine handle.
 * @param input User message.
 * @param on_token Token callback.
 * @param user_data Forwarded to callback.
 * @param cancel_flag Cancellation flag.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_run_streaming(
    entropic_handle_t handle,
    const char* input,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag) {
    (void)handle;
    (void)input;
    (void)on_token;
    (void)user_data;
    (void)cancel_flag;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Interrupt a running generation (stub).
 * @param handle Engine handle.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_interrupt(entropic_handle_t handle) {
    (void)handle;
    return ENTROPIC_ERROR_INVALID_STATE;
}

} // extern "C"
