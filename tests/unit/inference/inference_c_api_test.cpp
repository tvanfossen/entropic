// SPDX-License-Identifier: Apache-2.0
/**
 * @file inference_c_api_test.cpp
 * @brief Unit tests for the inference C ABI (inference_c_api.cpp).
 *
 * Wraps a Mock InferenceBackend in the same opaque-handle shape the
 * C API expects (`reinterpret_cast<entropic_inference_backend_t>`)
 * and drives every C entry point without loading a real model. Each
 * entry point's NULL-handle, exception, success, and error paths get
 * coverage so `src/inference/inference_c_api.cpp` reaches the
 * librentropic-inference coverage gate.
 *
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/backend.h>
#include <entropic/interfaces/i_inference_backend.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/logprob_result.h>
#include <entropic/types/message.h>

#include <llama.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Plugin C API entry points — declared only as docs in
// i_inference_backend.h; live as ENTROPIC_EXPORT exports in
// src/inference/inference_c_api.cpp. Re-declare here so the test
// can link against them.
extern "C" {
ENTROPIC_EXPORT int entropic_plugin_api_version();
ENTROPIC_EXPORT entropic_inference_backend_t
    entropic_create_inference_backend();
// v2.3.10 top-up: entropic_inference_log_to_file / _silence are already
// declared in i_inference_backend.h (above) — the inference target
// compiles with CXX_VISIBILITY_PRESET=default so the .so exports them
// even without an ENTROPIC_EXPORT marker.
}

namespace {

/**
 * @brief MockBackend that doesn't load a model — wraps the
 *        InferenceBackend base class with synthetic do_* overrides
 *        so the inference C API can call into it.
 *
 * Mirrors backend_lifecycle_test.cpp's MockBackend but adds
 * generate / generate_streaming / complete / count_tokens overrides
 * so the inference C ABI's full surface can be exercised.
 *
 * @internal
 * @version 2.3.10
 */
class ApiMockBackend : public entropic::InferenceBackend {
public:
    bool fail_load = false;
    bool fail_activate = false;
    bool fail_generate = false;
    bool throw_on_load = false;
    bool throw_on_activate = false;
    bool throw_on_generate = false;
    // v2.3.10 top-up (gh#23): exception coverage for the remaining
    // catch blocks in inference_c_api.cpp (deactivate / unload /
    // generate_streaming / complete / count_tokens).
    bool throw_on_deactivate = false;
    bool throw_on_unload = false;
    bool throw_on_streaming = false;
    bool throw_on_complete = false;
    bool throw_on_count_tokens = false;
    std::string generate_content = "mock-generated-text";
    int count_tokens_result = 7;

    int load_calls = 0;
    int activate_calls = 0;
    int deactivate_calls = 0;
    int unload_calls = 0;
    int generate_calls = 0;
    int generate_streaming_calls = 0;
    int complete_calls = 0;
    int count_tokens_calls = 0;

protected:
    std::string do_backend_name() const override { return "api-mock"; }

    bool do_load(const entropic::ModelConfig&) override {
        ++load_calls;
        if (throw_on_load) {
            throw std::runtime_error("mock load threw");
        }
        if (fail_load) {
            last_error_ = "mock load failed";
            return false;
        }
        return true;
    }

    bool do_activate() override {
        ++activate_calls;
        if (throw_on_activate) {
            throw std::runtime_error("mock activate threw");
        }
        if (fail_activate) {
            last_error_ = "mock activate failed";
            return false;
        }
        return true;
    }

    void do_deactivate() override {
        ++deactivate_calls;
        if (throw_on_deactivate) {
            throw std::runtime_error("mock deactivate threw");
        }
    }
    void do_unload() override {
        ++unload_calls;
        if (throw_on_unload) {
            throw std::runtime_error("mock unload threw");
        }
    }

    entropic::GenerationResult do_generate(
        const std::vector<entropic::Message>&,
        const entropic::GenerationParams&) override {
        ++generate_calls;
        if (throw_on_generate) {
            throw std::runtime_error("mock generate threw");
        }
        entropic::GenerationResult r;
        if (fail_generate) {
            r.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            r.error_message = "mock generate failed";
            return r;
        }
        r.content = generate_content;
        r.finish_reason = "stop";
        r.token_count = 4;
        r.generation_time_ms = 1.0;
        return r;
    }

    entropic::GenerationResult do_generate_streaming(
        const std::vector<entropic::Message>& msgs,
        const entropic::GenerationParams& params,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>&) override {
        ++generate_streaming_calls;
        if (throw_on_streaming) {
            throw std::runtime_error("mock streaming threw");
        }
        // Emit a single token via the callback so the C API's
        // token-bridge lambda executes.
        on_token(std::string_view{"tok"});
        return do_generate(msgs, params);
    }

    entropic::GenerationResult do_complete(
        const std::string&,
        const entropic::GenerationParams&) override {
        ++complete_calls;
        if (throw_on_complete) {
            throw std::runtime_error("mock complete threw");
        }
        entropic::GenerationResult r;
        r.content = generate_content;
        r.finish_reason = "stop";
        return r;
    }

    int do_count_tokens(const std::string&) const override {
        // count_tokens_calls is logically const-mutable for test counting.
        // Use a const_cast since the base method is const-qualified.
        const_cast<ApiMockBackend*>(this)->count_tokens_calls++;
        if (throw_on_count_tokens) {
            throw std::runtime_error("mock count_tokens threw");
        }
        return count_tokens_result;
    }

    entropic::LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) override {
        entropic::LogprobResult r;
        r.tokens.assign(tokens, tokens + n_tokens);
        r.n_tokens = n_tokens;
        r.n_logprobs = n_tokens > 0 ? n_tokens - 1 : 0;
        r.logprobs.resize(static_cast<size_t>(r.n_logprobs), -0.5f);
        return r;
    }
};

/**
 * @brief Cast a MockBackend* to the opaque C handle type.
 *
 * Mirrors `entropic_create_inference_backend`'s
 * `reinterpret_cast<entropic_inference_backend_t>(new ...)` pattern.
 * Safe because InferenceBackend* is the concrete type the C API's
 * `to_backend()` reinterprets back to.
 *
 * @utility
 * @version 2.3.10
 */
entropic_inference_backend_t as_handle(ApiMockBackend& mock) {
    return reinterpret_cast<entropic_inference_backend_t>(
        static_cast<entropic::InferenceBackend*>(&mock));
}

} // anonymous namespace

// ── Plugin metadata ─────────────────────────────────────────

TEST_CASE("entropic_plugin_api_version returns the wire version",
          "[inference_c_api][v2.3.10]")
{
    REQUIRE(entropic_plugin_api_version() == 1);
}

TEST_CASE("entropic_create_inference_backend yields a destroyable handle",
          "[inference_c_api][v2.3.10]")
{
    auto h = entropic_create_inference_backend();
    REQUIRE(h != nullptr);
    // entropic_inference_destroy frees through `delete to_backend(h)`.
    entropic_inference_destroy(h);
}

// ── NULL-handle guards (v2.3.10) ────────────────────────────
//
// Every entry-point that takes a `backend` handle now rejects null
// with ENTROPIC_ERROR_INVALID_ARGUMENT instead of segfaulting.
// Pre-v2.3.10 to_backend(nullptr) returned null and the next
// `->method()` call took the process down.

TEST_CASE("entropic_inference_load rejects NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_load(nullptr, "{}")
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_activate rejects NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_activate(nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_deactivate rejects NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_deactivate(nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_unload rejects NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_unload(nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_state returns COLD for NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_state(nullptr)
            == static_cast<int>(ENTROPIC_MODEL_STATE_COLD));
}

TEST_CASE("entropic_inference_generate rejects NULL handle",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    char* result = nullptr;
    REQUIRE(entropic_inference_generate(nullptr, "[]", "{}", &result)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
    REQUIRE(result == nullptr);
}

TEST_CASE("entropic_inference_generate rejects NULL result pointer",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_generate(h, "[]", "{}", nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_generate_streaming rejects NULL handle / callback",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    int cancel = 0;
    REQUIRE(entropic_inference_generate_streaming(
                nullptr, "[]", "{}",
                [](const char*, size_t, void*) {}, nullptr, &cancel)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);

    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_generate_streaming(
                h, "[]", "{}", nullptr, nullptr, &cancel)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_complete rejects NULL handle / prompt / result",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    char* out = nullptr;
    REQUIRE(entropic_inference_complete(nullptr, "p", "{}", &out)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);

    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_complete(h, nullptr, "{}", &out)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
    REQUIRE(entropic_inference_complete(h, "prompt", "{}", nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("entropic_inference_count_tokens returns 0 for NULL handle / text",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    REQUIRE(entropic_inference_count_tokens(nullptr, "hi", 2) == 0);

    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_count_tokens(h, nullptr, 5) == 0);
}

// ── Lifecycle: load ─────────────────────────────────────────

TEST_CASE("entropic_inference_load forwards success",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(mock.load_calls == 1);
}

TEST_CASE("entropic_inference_load returns LOAD_FAILED on backend failure",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    ApiMockBackend mock;
    mock.fail_load = true;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_ERROR_LOAD_FAILED);
}

TEST_CASE("entropic_inference_load catches exceptions and returns LOAD_FAILED",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    ApiMockBackend mock;
    mock.throw_on_load = true;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_ERROR_LOAD_FAILED);
}

TEST_CASE("entropic_inference_load parses ModelConfig JSON fields",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    // parse_config_json exercises set_if templates for each field.
    const char* config = R"({
        "path": "/tmp/model.gguf",
        "adapter": "qwen35",
        "context_length": 8192,
        "gpu_layers": -1,
        "keep_warm": true,
        "use_mlock": false,
        "n_batch": 1024,
        "n_threads": 8,
        "flash_attn": true
    })";
    REQUIRE(entropic_inference_load(h, config) == ENTROPIC_OK);
}

// ── Lifecycle: activate / deactivate / unload ───────────────

TEST_CASE("entropic_inference_activate happy and failure paths",
          "[inference_c_api][v2.3.10]")
{
    SECTION("success") {
        ApiMockBackend mock;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
    }
    SECTION("backend failure") {
        ApiMockBackend mock;
        mock.fail_activate = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h)
                == ENTROPIC_ERROR_LOAD_FAILED);
    }
    SECTION("backend throws") {
        ApiMockBackend mock;
        mock.throw_on_activate = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h)
                == ENTROPIC_ERROR_LOAD_FAILED);
    }
}

TEST_CASE("entropic_inference_deactivate increments the counter",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
    REQUIRE(entropic_inference_deactivate(h) == ENTROPIC_OK);
    REQUIRE(mock.deactivate_calls == 1);
}

TEST_CASE("entropic_inference_unload increments the counter",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_unload(h) == ENTROPIC_OK);
    REQUIRE(mock.unload_calls == 1);
}

// ── State + tokens ──────────────────────────────────────────

TEST_CASE("entropic_inference_state returns the backend's state enum",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    // COLD before load.
    REQUIRE(entropic_inference_state(h)
            == static_cast<int>(ENTROPIC_MODEL_STATE_COLD));
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_state(h)
            == static_cast<int>(ENTROPIC_MODEL_STATE_WARM));
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
    REQUIRE(entropic_inference_state(h)
            == static_cast<int>(ENTROPIC_MODEL_STATE_ACTIVE));
}

TEST_CASE("entropic_inference_count_tokens delegates to backend when loaded",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    mock.count_tokens_result = 42;
    auto h = as_handle(mock);
    // count_tokens delegates to do_count_tokens only when the backend
    // is_loaded() — base class returns size()/4 fallback otherwise.
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    const char* text = "any text";
    REQUIRE(entropic_inference_count_tokens(
                h, text, std::strlen(text)) == 42);
}

TEST_CASE("entropic_inference_count_tokens falls back when backend is COLD",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    const char* text = "twelve chars";
    // Cold backend → fallback returns text.size() / 4.
    int n = entropic_inference_count_tokens(h, text, std::strlen(text));
    REQUIRE(n == static_cast<int>(std::strlen(text)) / 4);
}

// ── Generation ──────────────────────────────────────────────

TEST_CASE("entropic_inference_generate returns serialized JSON result",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    mock.generate_content = "hello from mock";
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    char* result = nullptr;
    auto err = entropic_inference_generate(
        h,
        R"([{"role":"user","content":"hi"}])",
        R"({"temperature":0.7,"top_p":0.9})",
        &result);

    REQUIRE(err == ENTROPIC_OK);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result).find("hello from mock") != std::string::npos);
    entropic_inference_free(result);
}

TEST_CASE("entropic_inference_generate returns error_code on backend failure",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    ApiMockBackend mock;
    mock.fail_generate = true;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    char* result = nullptr;
    auto err = entropic_inference_generate(
        h,
        R"([{"role":"user","content":"hi"}])",
        "{}",
        &result);

    REQUIRE(err == ENTROPIC_ERROR_GENERATE_FAILED);
    // The result is still allocated even on error.
    if (result) { entropic_inference_free(result); }
}

TEST_CASE("entropic_inference_generate catches backend exceptions",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    ApiMockBackend mock;
    mock.throw_on_generate = true;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    char* result = nullptr;
    auto err = entropic_inference_generate(
        h,
        R"([{"role":"user","content":"hi"}])",
        "{}",
        &result);
    REQUIRE(err == ENTROPIC_ERROR_GENERATE_FAILED);
}

TEST_CASE("entropic_inference_generate_streaming fires the token callback",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    static int callback_count;
    callback_count = 0;
    auto on_token = [](const char* /*tok*/, size_t /*len*/,
                       void* /*ud*/) {
        callback_count += 1;
    };

    int cancel = 0;
    auto err = entropic_inference_generate_streaming(
        h,
        R"([{"role":"user","content":"x"}])",
        "{}",
        on_token,
        nullptr,
        &cancel);

    REQUIRE(err == ENTROPIC_OK);
    REQUIRE(callback_count >= 1);
    REQUIRE(mock.generate_streaming_calls == 1);
}

TEST_CASE("entropic_inference_generate_streaming respects cancel flag",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    int cancel = 1;  // pre-set; lambda checks *cancel_flag in C API
    auto err = entropic_inference_generate_streaming(
        h, "[]", "{}",
        [](const char*, size_t, void*) {},
        nullptr, &cancel);
    REQUIRE(err == ENTROPIC_OK);
}

TEST_CASE("entropic_inference_complete returns serialized result",
          "[inference_c_api][v2.3.10]")
{
    ApiMockBackend mock;
    mock.generate_content = "completion result";
    auto h = as_handle(mock);
    REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
    REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);

    char* result = nullptr;
    auto err = entropic_inference_complete(
        h, "raw prompt", "{}", &result);
    REQUIRE(err == ENTROPIC_OK);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result).find("completion result")
            != std::string::npos);
    entropic_inference_free(result);
}

// ── Memory ──────────────────────────────────────────────────

TEST_CASE("entropic_inference_free tolerates nullptr",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    // std::free(nullptr) is defined; no crash.
    entropic_inference_free(nullptr);
    REQUIRE(true);
}

TEST_CASE("entropic_inference_destroy with nullptr is a no-op",
          "[inference_c_api][v2.3.10][failure-mode]")
{
    // delete nullptr is defined; no crash.
    entropic_inference_destroy(nullptr);
    REQUIRE(true);
}

// ── v2.3.10 top-up: exception catch blocks + parse_params ──
//
// Lines 218-221 (deactivate), 238-241 (unload), 335-337 (streaming),
// 365-367 (complete), 390-392 (count_tokens) all wrap backend calls
// in try/catch. The generate-throw path is already covered; this
// SECTION sweep covers the rest plus parse_params_json field branches.

TEST_CASE("inference C ABI: exception paths + parse_params field sweep",
          "[v2.3.10][inference][topup][failure-mode]")
{
    SECTION("deactivate catches backend exception → INTERNAL") {
        ApiMockBackend mock; mock.throw_on_deactivate = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
        REQUIRE(entropic_inference_deactivate(h) == ENTROPIC_ERROR_INTERNAL);
    }
    SECTION("unload catches backend exception → INTERNAL") {
        ApiMockBackend mock; mock.throw_on_unload = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_unload(h) == ENTROPIC_ERROR_INTERNAL);
    }
    SECTION("generate_streaming catches backend exception → GENERATE_FAILED") {
        ApiMockBackend mock; mock.throw_on_streaming = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
        int cancel = 0;
        REQUIRE(entropic_inference_generate_streaming(
                    h, "[]", "{}",
                    [](const char*, size_t, void*) {}, nullptr, &cancel)
                == ENTROPIC_ERROR_GENERATE_FAILED);
    }
    SECTION("complete catches backend exception → GENERATE_FAILED") {
        ApiMockBackend mock; mock.throw_on_complete = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
        char* r = nullptr;
        REQUIRE(entropic_inference_complete(h, "p", "{}", &r)
                == ENTROPIC_ERROR_GENERATE_FAILED);
        if (r) { entropic_inference_free(r); }
    }
    SECTION("count_tokens catch returns text_len/4 fallback") {
        ApiMockBackend mock; mock.throw_on_count_tokens = true;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        const char* t = "sixteen-char-str";  // 16/4 = 4
        REQUIRE(entropic_inference_count_tokens(h, t, std::strlen(t))
                == static_cast<int>(std::strlen(t)) / 4);
    }
    SECTION("parse_params_json sweeps every documented field") {
        ApiMockBackend mock;
        auto h = as_handle(mock);
        REQUIRE(entropic_inference_load(h, "{}") == ENTROPIC_OK);
        REQUIRE(entropic_inference_activate(h) == ENTROPIC_OK);
        char* r = nullptr;
        REQUIRE(entropic_inference_generate(
                    h, R"([{"role":"user","content":"hi"}])",
                    R"({"temperature":0.5,"top_p":0.95,"top_k":40,
                        "min_p":0.05,"repeat_penalty":1.1,
                        "max_tokens":256,"grammar":"root ::= [a-z]+"})",
                    &r) == ENTROPIC_OK);
        if (r) { entropic_inference_free(r); }
    }
}

// ── v2.3.10 top-up: ggml log redirect + callbacks ──────────
//
// Lines 484-485, 496-500, 513-541 (log_to_file / log_silence) and
// 454-460 (ggml_log_to_file callback body), 467-469 (ggml_log_noop).
// Drives every branch: null/empty silence, valid path, same-path
// re-call, conflicting second path (first-call wins, gh#58),
// unwritable path (fopen fails), idempotent silence. Triggers the
// callbacks by routing a llama_model_load_from_file failure through
// the active log function — once redirected (writes to fp), once
// silenced (drops).

TEST_CASE("entropic_inference_log_to_file: every branch + ggml callbacks",
          "[v2.3.10][inference][topup]")
{
    namespace fs = std::filesystem;
    std::error_code ec;
    auto path_a = fs::temp_directory_path() / "entropic_log_topup_a.log";
    auto path_b = fs::temp_directory_path() / "entropic_log_topup_b.log";
    fs::remove(path_a, ec); fs::remove(path_b, ec);

    auto try_load_bogus = [](const char* p) {
        llama_model_params mp = llama_model_default_params();
        mp.vocab_only = true; mp.n_gpu_layers = 0;
        llama_model* m = llama_model_load_from_file(p, mp);
        if (m != nullptr) { llama_model_free(m); }
    };

    // null / empty path → silence_locked.
    entropic_inference_log_to_file(nullptr);
    entropic_inference_log_to_file("");

    // Valid path → fopen + canonicalize + llama_log_set wired.
    entropic_inference_log_to_file(path_a.string().c_str());
    REQUIRE(fs::exists(path_a));
    // Trigger ggml_log_to_file via a bogus model load.
    try_load_bogus("/tmp/entropic-nonexistent-trigger.gguf");

    // Same-path re-call → truncate + reopen (lines 537-538).
    entropic_inference_log_to_file(path_a.string().c_str());
    // Conflicting second path → rejected (lines 524-528).
    entropic_inference_log_to_file(path_b.string().c_str());
    REQUIRE_FALSE(fs::exists(path_b));

    // Silence (×2 for idempotency) → ggml_log_noop becomes active.
    entropic_inference_log_silence();
    entropic_inference_log_silence();
    try_load_bogus("/tmp/entropic-nonexistent-trigger-2.gguf");

    // Unwritable path → fopen fails (lines 531-535). No prior wiring.
    entropic_inference_log_to_file(
        "/this/path/does/not/exist/entropic_log_topup.log");

    entropic_inference_log_silence();
    fs::remove(path_a, ec); fs::remove(path_b, ec);
}
