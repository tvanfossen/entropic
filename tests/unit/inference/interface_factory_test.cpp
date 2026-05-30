// SPDX-License-Identifier: Apache-2.0
/**
 * @file interface_factory_test.cpp
 * @brief Coverage top-up for src/inference/interface_factory.cpp —
 *        drives every C callback (parse_msgs / parse_params /
 *        extract_tier / iface_*) via a default-constructed
 *        ModelOrchestrator (returns build_no_model_error → no crash).
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/interface_factory.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/interfaces/i_inference_callbacks.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct InterfaceFixture {
    entropic::ModelOrchestrator orch;
    entropic::InterfaceContext* ctx = nullptr;
    entropic::InferenceInterface iface;
    explicit InterfaceFixture(const std::string& tier = "primary") {
        iface = entropic::build_orchestrator_interface(&orch, tier, &ctx);
    }
    ~InterfaceFixture() { entropic::destroy_orchestrator_interface(ctx); }
    InterfaceFixture(const InterfaceFixture&) = delete;
    InterfaceFixture& operator=(const InterfaceFixture&) = delete;
};
} // anonymous namespace

TEST_CASE("interface_factory: wiring + iface_* surface sweep",
          "[v2.3.10][inference][topup][interface_factory]")
{
    SECTION("build_orchestrator_interface wires every slot; destroy(nullptr) safe") {
        InterfaceFixture fx;
        REQUIRE(fx.iface.generate != nullptr);
        REQUIRE(fx.iface.generate_stream != nullptr);
        REQUIRE(fx.iface.route != nullptr);
        REQUIRE(fx.iface.complete != nullptr);
        REQUIRE(fx.iface.parse_tool_calls != nullptr);
        REQUIRE(fx.iface.is_response_complete != nullptr);
        REQUIRE(fx.iface.free_fn != nullptr);
        REQUIRE(fx.iface.backend_data == fx.ctx);
        entropic::destroy_orchestrator_interface(nullptr);
    }

    SECTION("iface_generate drives parse_msgs / parse_params / extract_tier") {
        InterfaceFixture fx("default-tier");
        char* out = nullptr;
        // Every parse_params field branch + explicit tier.
        const char* full = R"({
            "max_tokens": 16, "temperature": 0.7, "grammar_key": "json",
            "enable_thinking": true, "top_p": 0.9, "top_k": 40,
            "min_p": 0.05, "repeat_penalty": 1.1, "seed": 42, "tier": "x"
        })";
        REQUIRE(fx.iface.generate(
            R"([{"role":"user","content":"hi"}])",
            full, &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out); out = nullptr;
        // Null msgs/params → parse_* return defaults.
        REQUIRE(fx.iface.generate(
            nullptr, nullptr, &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out); out = nullptr;
        // Malformed JSON → discarded → defaults.
        REQUIRE(fx.iface.generate(
            "not json", "also not", &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out); out = nullptr;
        // Non-object params (extract_tier fallback).
        REQUIRE(fx.iface.generate(
            "[]", "[1,2,3]", &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out); out = nullptr;
        // Missing-tier branch.
        REQUIRE(fx.iface.generate(
            "[]", R"({"max_tokens":1})",
            &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out);
    }

    SECTION("iface_generate_stream + cancel flag + route + complete") {
        InterfaceFixture fx("fallback");
        int cancel = 0;
        REQUIRE(fx.iface.generate_stream(
            R"([{"role":"user","content":"x"}])",
            R"({"max_tokens":1})",
            [](const char*, size_t, void*) {}, nullptr, &cancel,
            fx.iface.backend_data) == 0);
        int pre = 1;
        REQUIRE(fx.iface.generate_stream(
            "[]", "{}", [](const char*, size_t, void*) {},
            nullptr, &pre, fx.iface.backend_data) == 0);

        char* out = nullptr;
        REQUIRE(fx.iface.route(
            R"([{"role":"user","content":"x"}])",
            &out, fx.iface.backend_data) == 0);
        REQUIRE(out != nullptr);
        fx.iface.free_fn(out); out = nullptr;

        // complete: explicit tier override and null params.
        REQUIRE(fx.iface.complete(
            "prompt", R"({"tier":"override"})",
            &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out); out = nullptr;
        REQUIRE(fx.iface.complete(
            "p", nullptr, &out, fx.iface.backend_data) == 0);
        fx.iface.free_fn(out);
    }

    SECTION("iface_parse_tool_calls null-adapter branch") {
        InterfaceFixture fx;
        char* cleaned = nullptr;
        char* calls = nullptr;
        REQUIRE(fx.iface.parse_tool_calls(
            "raw", &cleaned, &calls, fx.iface.backend_data) == 0);
        REQUIRE(std::string(cleaned) == "raw");
        REQUIRE(std::string(calls) == "[]");
        fx.iface.free_fn(cleaned); fx.iface.free_fn(calls);
        cleaned = nullptr; calls = nullptr;
        // Null raw → cleaned empty.
        REQUIRE(fx.iface.parse_tool_calls(
            nullptr, &cleaned, &calls, fx.iface.backend_data) == 0);
        REQUIRE(std::string(cleaned).empty());
        fx.iface.free_fn(cleaned); fx.iface.free_fn(calls);
    }

    SECTION("iface_is_complete: 1 for null/empty/malformed, 0 for non-empty") {
        InterfaceFixture fx;
        REQUIRE(fx.iface.is_response_complete(
            "x", nullptr, fx.iface.adapter_data) == 1);
        REQUIRE(fx.iface.is_response_complete(
            "x", "[]", fx.iface.adapter_data) == 1);
        REQUIRE(fx.iface.is_response_complete(
            "x", "{not}", fx.iface.adapter_data) == 1);
        REQUIRE(fx.iface.is_response_complete(
            "x", R"([{"name":"f","arguments":{}}])",
            fx.iface.adapter_data) == 0);
    }

    // gh#87 (v2.7.0): iface_parse_tool_calls adapter branch + the
    // serialize_tool_calls helper. create_tier_backends checks the model
    // path EXISTS then creates the adapter, BEFORE activate_default_tier
    // loads it. So a dummy *existing* (non-GGUF) file: the qwen35 adapter
    // is created, activate fails cleanly (invalid GGUF → null, no model
    // loaded → no SEGV), init returns false, and get_adapter returns a
    // live adapter. The backend is COLD (common_chat_parse_reliable() →
    // false), so parsing routes to the adapter — exercising the adapter
    // branch and both serialize_tool_calls arg branches (string-fallback
    // for "config.yaml" + JSON-number for 10), no real model load.
    SECTION("iface_parse_tool_calls adapter branch + serialize_tool_calls") {
        auto dummy = std::filesystem::temp_directory_path() /
                     "entropic_cov_dummy_model.gguf";
        { std::ofstream(dummy) << "not a real gguf"; }

        entropic::ModelOrchestrator orch;
        entropic::ParsedConfig cfg;
        cfg.models.default_tier = "primary";
        entropic::TierConfig lead;
        lead.path = dummy;
        lead.adapter = "qwen35";
        cfg.models.tiers["primary"] = lead;
        (void)orch.initialize(cfg);  // false (invalid GGUF); adapter created

        entropic::InterfaceContext* ctx = nullptr;
        auto iface = entropic::build_orchestrator_interface(
            &orch, "primary", &ctx);

        char* cleaned = nullptr;
        char* calls = nullptr;
        REQUIRE(iface.parse_tool_calls(
            "<function=read_file>\n"
            "<parameter=path>config.yaml</parameter>\n"
            "<parameter=lines>10</parameter>\n</function>",
            &cleaned, &calls, iface.backend_data) == 0);
        REQUIRE(cleaned != nullptr);
        REQUIRE(calls != nullptr);
        // Adapter extracted the multi-param call → non-empty serialized array.
        CHECK(std::string(calls) != "[]");
        iface.free_fn(cleaned);
        iface.free_fn(calls);
        entropic::destroy_orchestrator_interface(ctx);
        std::filesystem::remove(dummy);
    }
}
