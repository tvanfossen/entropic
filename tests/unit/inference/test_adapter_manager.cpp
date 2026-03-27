/**
 * @file test_adapter_manager.cpp
 * @brief Tests for AdapterManager LoRA lifecycle and hot-swap.
 *
 * Uses mock llama.cpp adapter functions via link-time substitution.
 * The real llama.cpp adapter API is not available in unit tests
 * (no GPU, no model files). Instead, the mock tracks calls and
 * returns synthetic handles.
 *
 * @version 1.9.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/adapter_manager.h>
#include <entropic/types/hooks.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// ── Mock llama.cpp adapter API ──────────────────────────────
//
// These replace the real llama.cpp symbols at link time for tests.
// Each function tracks call counts and returns synthetic handles.

namespace mock_adapter {

static int init_calls = 0;
static int set_calls = 0;
static int free_calls = 0;
static bool fail_init = false;

/**
 * @brief Reset all mock counters.
 * @utility
 * @version 1.9.2
 */
void reset() {
    init_calls = 0;
    set_calls = 0;
    free_calls = 0;
    fail_init = false;
}

} // namespace mock_adapter

// Mock implementations matching real llama.cpp API (pinned b8420)
extern "C" {

/**
 * @brief Mock llama_adapter_lora_init.
 * @param model Base model (ignored in mock).
 * @param path Adapter path (ignored in mock).
 * @return Synthetic handle, or nullptr if fail_init is set.
 * @callback
 * @version 1.9.2
 */
struct llama_adapter_lora* llama_adapter_lora_init(
    struct llama_model* /*model*/, const char* /*path*/)
{
    ++mock_adapter::init_calls;
    if (mock_adapter::fail_init) {
        return nullptr;
    }
    // Return a non-null synthetic handle (never dereferenced)
    static int handles[32];
    return reinterpret_cast<struct llama_adapter_lora*>(
        &handles[mock_adapter::init_calls % 32]);
}

/**
 * @brief Mock llama_set_adapters_lora (plural — real API).
 * @param ctx Context (ignored).
 * @param adapters Array of adapter handles (ignored).
 * @param n_adapters Count (ignored).
 * @param scales Scale factors (ignored).
 * @return 0 on success.
 * @callback
 * @version 1.9.2
 */
int32_t llama_set_adapters_lora(
    struct llama_context* /*ctx*/,
    struct llama_adapter_lora** /*adapters*/,
    size_t /*n_adapters*/,
    float* /*scales*/)
{
    ++mock_adapter::set_calls;
    return 0;
}

/**
 * @brief Mock llama_adapter_lora_free.
 * @param adapter Adapter handle (ignored).
 * @callback
 * @version 1.9.2
 */
void llama_adapter_lora_free(struct llama_adapter_lora* /*adapter*/) {
    ++mock_adapter::free_calls;
}

} // extern "C"

// ── Synthetic handles ───────────────────────────────────────

namespace {

/**
 * @brief Get a synthetic llama_model pointer for testing.
 * @return Non-null fake pointer (never dereferenced).
 * @utility
 * @version 1.9.2
 */
llama_model* fake_model() {
    static int m;
    return reinterpret_cast<llama_model*>(&m);
}

/**
 * @brief Get a second synthetic llama_model pointer.
 * @return Non-null fake pointer distinct from fake_model().
 * @utility
 * @version 1.9.2
 */
llama_model* fake_model_2() {
    static int m;
    return reinterpret_cast<llama_model*>(&m);
}

/**
 * @brief Get a synthetic llama_context pointer for testing.
 * @return Non-null fake pointer (never dereferenced).
 * @utility
 * @version 1.9.2
 */
llama_context* fake_ctx() {
    static int c;
    return reinterpret_cast<llama_context*>(&c);
}

} // anonymous namespace

// ── Tests ───────────────────────────────────────────────────

TEST_CASE("AdapterManager load transitions COLD to WARM", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;

    REQUIRE(mgr.state("eng") == entropic::AdapterState::COLD);

    bool ok = mgr.load("eng", "/path/eng-lora.gguf", fake_model(), 1.0f);

    REQUIRE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::WARM);
    REQUIRE(mock_adapter::init_calls == 1);

    auto info = mgr.info("eng");
    REQUIRE(info.name == "eng");
    REQUIRE(info.scale == 1.0f);
}

TEST_CASE("AdapterManager activate transitions WARM to HOT", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());

    bool ok = mgr.activate("eng", fake_ctx());

    REQUIRE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::HOT);
    REQUIRE(mgr.active_adapter() == "eng");
    REQUIRE(mock_adapter::set_calls == 1);
}

TEST_CASE("Activate deactivates current HOT adapter", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.load("qa", "/path/qa-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    bool ok = mgr.activate("qa", fake_ctx());

    REQUIRE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::WARM);
    REQUIRE(mgr.state("qa") == entropic::AdapterState::HOT);
    REQUIRE(mgr.active_adapter() == "qa");
}

TEST_CASE("Deactivate transitions HOT to WARM", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    mgr.deactivate(fake_ctx());

    REQUIRE(mgr.state("eng") == entropic::AdapterState::WARM);
    REQUIRE(mgr.active_adapter().empty());
}

TEST_CASE("Unload transitions any state to COLD", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());
    REQUIRE(mgr.state("eng") == entropic::AdapterState::HOT);

    mgr.unload("eng", fake_ctx());

    REQUIRE(mgr.state("eng") == entropic::AdapterState::COLD);
    REQUIRE(mgr.active_adapter().empty());
    REQUIRE(mock_adapter::free_calls == 1);
}

TEST_CASE("Swap atomically switches adapters", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.load("qa", "/path/qa-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    bool ok = mgr.swap("qa", fake_ctx());

    REQUIRE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::WARM);
    REQUIRE(mgr.state("qa") == entropic::AdapterState::HOT);
    REQUIRE(mgr.active_adapter() == "qa");
}

TEST_CASE("Swap on COLD adapter fails", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    // "qa" not loaded — COLD
    bool ok = mgr.swap("qa", fake_ctx());

    REQUIRE_FALSE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::HOT);
    REQUIRE(mgr.active_adapter() == "eng");
}

TEST_CASE("Unload all for model clears all adapters", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());
    mgr.load("qa", "/path/qa-lora.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    mgr.unload_all_for_model(fake_model(), fake_ctx());

    REQUIRE(mgr.state("eng") == entropic::AdapterState::COLD);
    REQUIRE(mgr.state("qa") == entropic::AdapterState::COLD);
    REQUIRE(mgr.active_adapter().empty());
    REQUIRE(mock_adapter::free_calls == 2);
}

TEST_CASE("Load duplicate name fails", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng-lora.gguf", fake_model());

    bool ok = mgr.load("eng", "/different/path.gguf", fake_model());

    REQUIRE_FALSE(ok);
    REQUIRE(mock_adapter::init_calls == 1);
}

TEST_CASE("Load with null model fails", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;

    bool ok = mgr.load("eng", "/path/eng-lora.gguf", nullptr);

    REQUIRE_FALSE(ok);
    REQUIRE(mock_adapter::init_calls == 0);
}

TEST_CASE("Load with failing init returns false", "[adapter]") {
    mock_adapter::reset();
    mock_adapter::fail_init = true;
    entropic::AdapterManager mgr;

    bool ok = mgr.load("eng", "/path/eng-lora.gguf", fake_model());

    REQUIRE_FALSE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::COLD);
}

TEST_CASE("Activate unknown adapter fails", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;

    bool ok = mgr.activate("nonexistent", fake_ctx());

    REQUIRE_FALSE(ok);
}

TEST_CASE("list_adapters returns all managed adapters", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng.gguf", fake_model(), 1.0f);
    mgr.load("qa", "/path/qa.gguf", fake_model(), 0.8f);

    auto list = mgr.list_adapters();

    REQUIRE(list.size() == 2);
}

TEST_CASE("Swap to already active adapter is no-op", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng.gguf", fake_model());
    mgr.activate("eng", fake_ctx());
    int set_before = mock_adapter::set_calls;

    bool ok = mgr.swap("eng", fake_ctx());

    REQUIRE(ok);
    REQUIRE(mock_adapter::set_calls == set_before);
}

TEST_CASE("ON_ADAPTER_SWAP hook fires during swap", "[adapter][hooks]") {
    mock_adapter::reset();

    static bool hook_fired = false;

    /**
     * @brief Test fire_pre for adapter swap hook.
     * @callback
     * @version 1.9.2
     */
    auto fire_pre = [](void* /*registry*/,
                       entropic_hook_point_t /*point*/,
                       const char* context_json,
                       char** /*modified_json*/) -> int {
        hook_fired = true;
        // Verify context contains adapter names
        std::string ctx(context_json);
        REQUIRE(ctx.find("eng") != std::string::npos);
        REQUIRE(ctx.find("qa") != std::string::npos);
        return 0;  // Allow swap
    };

    entropic::HookInterface hooks;
    static int fake_registry;
    hooks.registry = &fake_registry;
    hooks.fire_pre = fire_pre;

    entropic::AdapterManager mgr;
    mgr.set_hook_interface(hooks);
    mgr.load("eng", "/path/eng.gguf", fake_model());
    mgr.load("qa", "/path/qa.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    hook_fired = false;
    bool ok = mgr.swap("qa", fake_ctx());

    REQUIRE(ok);
    REQUIRE(hook_fired);
}

TEST_CASE("ON_ADAPTER_SWAP hook can cancel swap", "[adapter][hooks]") {
    mock_adapter::reset();

    /**
     * @brief Cancelling fire_pre for adapter swap test.
     * @callback
     * @version 1.9.2
     */
    auto cancel_fire_pre = [](void* /*registry*/,
                              entropic_hook_point_t /*point*/,
                              const char* /*context_json*/,
                              char** /*modified_json*/) -> int {
        return 1;  // Cancel
    };

    entropic::HookInterface hooks;
    static int fake_registry;
    hooks.registry = &fake_registry;
    hooks.fire_pre = cancel_fire_pre;

    entropic::AdapterManager mgr;
    mgr.set_hook_interface(hooks);
    mgr.load("eng", "/path/eng.gguf", fake_model());
    mgr.load("qa", "/path/qa.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    bool ok = mgr.swap("qa", fake_ctx());

    REQUIRE_FALSE(ok);
    REQUIRE(mgr.state("eng") == entropic::AdapterState::HOT);
    REQUIRE(mgr.state("qa") == entropic::AdapterState::WARM);
    REQUIRE(mgr.active_adapter() == "eng");
}

TEST_CASE("State queries during swap are thread-safe", "[adapter][threads]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng.gguf", fake_model());
    mgr.load("qa", "/path/qa.gguf", fake_model());
    mgr.activate("eng", fake_ctx());

    std::atomic<bool> done{false};
    std::vector<entropic::AdapterState> observed_states;

    // Reader thread — queries state repeatedly
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            auto s = mgr.state("eng");
            observed_states.push_back(s);
        }
    });

    // Writer — swap multiple times
    for (int i = 0; i < 10; ++i) {
        mgr.swap("qa", fake_ctx());
        mgr.swap("eng", fake_ctx());
    }

    done.store(true, std::memory_order_release);
    reader.join();

    // All observed states must be valid enum values
    for (auto s : observed_states) {
        bool valid = s == entropic::AdapterState::COLD
                  || s == entropic::AdapterState::WARM
                  || s == entropic::AdapterState::HOT;
        REQUIRE(valid);
    }
}

TEST_CASE("Unload all ignores adapters for different model", "[adapter]") {
    mock_adapter::reset();
    entropic::AdapterManager mgr;
    mgr.load("eng", "/path/eng.gguf", fake_model());
    mgr.load("other", "/path/other.gguf", fake_model_2());

    mgr.unload_all_for_model(fake_model(), fake_ctx());

    REQUIRE(mgr.state("eng") == entropic::AdapterState::COLD);
    REQUIRE(mgr.state("other") == entropic::AdapterState::WARM);
}
