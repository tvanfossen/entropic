// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_mtp_stream_grammar.cpp
 * @brief gh#108 (v2.10.3): MTP × grammar × streaming through the REAL
 *        orchestrator streaming path.
 *
 * @par Why this file exists — a v2.10.0 release failure
 * v2.10.0 removed the grammar guard (decision #52) and the streaming guard
 * (decision #53) from `mtp_unsupported_reason`, and its release notes claimed
 * "`speculative.mtp: true` is now compatible with streaming calls." The
 * evidence behind that claim was insufficient:
 *
 *  - `test_gh108_mtp_guards.cpp` proved grammar+MTP with `no_stream` — an
 *    EMPTY std::function. Non-streaming.
 *  - It proved streaming+MTP by handing a raw lambda straight to
 *    `backend.generate_mtp(...)`, which bypasses the orchestrator entirely.
 *    That asserts the GUARD no longer fires; it does not exercise streaming.
 *  - `test_gh106_mtp_route.cpp` drives the non-streaming
 *    `orchestrator->generate()` overload.
 *  - No model test drove `orchestrator->generate_streaming` with MTP enabled
 *    at all, and grammar+streaming were never combined.
 *
 * So the two things v2.10.0 actually ADDED to the streaming path —
 * `StreamThinkFilter` wrapping and the `apply_adapter_parse` call (decision
 * #53) — had never executed under MTP. The suite passed over a vacuous test,
 * which is worse than a red suite: it certified a claim nothing checked.
 *
 * @par What this test exercises
 * The production route a streaming consumer actually hits:
 * @code
 *   orchestrator->generate_streaming()
 *     → StreamThinkFilter + stream_token_trampoline wrap on_token   (v2.10.0)
 *       → try_speculative_route_streaming → try_mtp_route → generate_mtp
 *     → filter.flush() → apply_adapter_parse(result.content)        (v2.10.0)
 * @endcode
 *
 * The decisive assertion is stream↔content AGREEMENT. `StreamThinkFilter`
 * mutates what the consumer sees live; `apply_adapter_parse` mutates the
 * buffered `result.content` afterwards. Nothing has ever checked the two agree.
 * A consumer that renders the stream to a UI and then stores `result.content`
 * would silently persist a different answer than the operator watched appear.
 *
 * @version 2.10.3
 */

#include "model_test_context.h"  // helpers only — NO CATCH_REGISTER_LISTENER

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

std::filesystem::path models_dir() {
    return std::filesystem::path(getenv("HOME")) / ".entropic" / "models";
}

/**
 * @brief Point the default tier at the gemma4 MTP target + head, MTP on.
 *
 * Mirrors configure_mtp_route in test_gh106_mtp_route.cpp so the two files
 * agree on what "an MTP-effective tier" means.
 *
 * @param ctx Test context (config mutated in place).
 * @return Default tier name.
 * @version 2.10.3
 */
std::string configure_mtp_tier(ModelTestContext& ctx) {
    auto target = models_dir() / "gemma-4-E2B-it-Q8_0.gguf";
    auto head = models_dir() / "mtp-gemma-4-E2B-it.gguf";
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }
    REQUIRE(load_registry(ctx.registry));
    REQUIRE(load_test_config(ctx.registry, ctx.config));
    auto tier_name = ctx.config.models.default_tier;
    auto it = ctx.config.models.tiers.find(tier_name);
    if (it == ctx.config.models.tiers.end()) {
        SKIP("no default tier in config to repoint");
    }
    auto& tier = it->second;
    tier.path = target;
    tier.adapter = "gemma4";
    tier.gpu_layers = 99;
    tier.context_length = 4096;
    tier.flash_attn = false;
    tier.cache_type_k = "f16";
    tier.cache_type_v = "f16";
    tier.grammar.reset();  // grammar arrives per-request, not statically
    auto& spec = ctx.config.inference.speculative;
    spec.enabled = true;
    spec.mtp = true;
    spec.draft.path = head;
    spec.n_draft = 16;
    return tier_name;
}

/**
 * @brief Thread-safe accumulator for streamed tokens.
 *
 * on_token is invoked from the decode loop; the callback is documented as
 * serialised per call, but the accumulator is guarded anyway so a future
 * threading change surfaces as a test failure rather than a data race.
 *
 * @version 2.10.3
 */
struct StreamCapture {
    std::mutex mu;       ///< Guards the fields below
    std::string text;    ///< Concatenation of every token delivered
    int token_count{0};  ///< Number of on_token invocations

    /**
     * @brief Append one streamed token.
     * @param tok Token text.
     * @version 2.10.3
     */
    void push(std::string_view tok) {
        std::lock_guard<std::mutex> lock(mu);
        text.append(tok);
        ++token_count;
    }
};

}  // namespace

TEST_CASE("gh#108 (v2.10.3) MTP + grammar + streaming through "
          "orchestrator->generate_streaming",
          "[model][gh108][mtp][streaming][grammar][combined]") {
    ModelTestContext ctx;
    auto tier_name = configure_mtp_tier(ctx);
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Say ok.";

    entropic::GenerationParams params;
    params.max_tokens = 32;
    params.temperature = 0.0f;
    // A grammar with exactly one accepting string makes correctness decidable
    // without depending on the model's free-form phrasing.
    params.grammar = "root ::= \"ok\"";

    StreamCapture cap;
    std::atomic<bool> cancel{false};
    auto on_token = [&cap](std::string_view tok) { cap.push(tok); };

    auto r = ctx.orchestrator->generate_streaming({u}, params, on_token,
                                                  cancel, tier_name);

    std::printf("\n===gh108 MTP+grammar+streaming===\n"
                "code=%d drafted=%d accepted=%d tokens_streamed=%d\n"
                "streamed=[%s]\ncontent =[%s]\n===\n",
                r.error_code, r.n_drafted, r.n_accepted, cap.token_count,
                cap.text.c_str(), r.content.c_str());

    REQUIRE(r.error_code == 0);

    // (1) MTP actually engaged. Without this the whole test passes vacuously
    // via the plain-decode fallback — the exact failure mode that let v2.10.0
    // ship an unproven claim. n_drafted is non-zero only when generate_mtp ran.
    REQUIRE(r.n_drafted > 0);

    // (2) The consumer received a live stream at all. StreamThinkFilter sits
    // between the decode loop and on_token; if it swallowed everything, this
    // is where that shows.
    REQUIRE(cap.token_count > 0);
    REQUIRE_FALSE(cap.text.empty());

    // (3) Grammar was enforced on the MTP sampler chain (decision #52) and
    // survived the filter on the way to the consumer.
    REQUIRE(cap.text.find("ok") != std::string::npos);
    REQUIRE(r.content.find("ok") != std::string::npos);

    // (4) THE DECISIVE ONE. StreamThinkFilter mutates the streamed view;
    // apply_adapter_parse mutates result.content afterwards. If they disagree,
    // a consumer rendering the stream live and then persisting result.content
    // stores a different answer than the operator watched appear.
    CHECK(cap.text == r.content);

    // (5) Incremental channel stripping did its job — no raw thinking-channel
    // markers reached the consumer. This is the reason the streaming guard
    // existed before v2.10.0, so it must be proven, not assumed.
    CHECK(cap.text.find("<|channel") == std::string::npos);
    CHECK(r.content.find("<|channel") == std::string::npos);
}

TEST_CASE("gh#108 (v2.10.3) MTP + streaming without a grammar still streams",
          "[model][gh108][mtp][streaming][combined]") {
    // Isolates the streaming axis through the real path, so a failure in the
    // combined case above can be attributed to grammar rather than to
    // streaming being broken generally.
    ModelTestContext ctx;
    auto tier_name = configure_mtp_tier(ctx);
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Count to five, one number per line.";

    entropic::GenerationParams params;
    // gh#108 (v2.10.3): 48 was too tight. Once the channel leak was fixed,
    // gemma4 was still INSIDE its reasoning block at the budget, so the
    // (correct) suppression left nothing to stream and the assertions below
    // failed on the test's own under-budgeting rather than on the engine.
    params.max_tokens = 500;
    params.temperature = 0.0f;

    StreamCapture cap;
    std::atomic<bool> cancel{false};
    auto on_token = [&cap](std::string_view tok) { cap.push(tok); };

    auto r = ctx.orchestrator->generate_streaming({u}, params, on_token,
                                                  cancel, tier_name);

    std::printf("\n===gh108 MTP+streaming (no grammar)===\n"
                "code=%d drafted=%d tokens_streamed=%d\n"
                "streamed=[%s]\ncontent =[%s]\n===\n",
                r.error_code, r.n_drafted, cap.token_count,
                cap.text.c_str(), r.content.c_str());

    REQUIRE(r.error_code == 0);
    REQUIRE(r.n_drafted > 0);       // MTP engaged, not plain-decode fallback
    REQUIRE(cap.token_count > 0);   // tokens actually reached the consumer
    CHECK(cap.text == r.content);   // stream and buffer agree
    CHECK(cap.text.find("<|channel") == std::string::npos);
}

TEST_CASE("gh#108 (v2.10.3) CONTROL: plain-decode streaming on the same tier",
          "[model][gh108][streaming][control]") {
    // Isolates whether channel leakage is an MTP property at all.
    //
    // If plain decode leaks <|channel> identically, then the v2.9.1 streaming
    // guard never protected anyone: it blocked MTP for a defect that belongs
    // to streaming+gemma4 generally, and v2.10.0 removing it did not introduce
    // the leak. If plain decode is clean, the leak IS MTP-specific.
    ModelTestContext ctx;
    auto tier_name = configure_mtp_tier(ctx);
    ctx.config.inference.speculative.enabled = false;  // the only difference
    ctx.config.inference.speculative.mtp = false;
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Count to five, one number per line.";

    entropic::GenerationParams params;
    // gh#108 (v2.10.3): 48 was too tight. Once the channel leak was fixed,
    // gemma4 was still INSIDE its reasoning block at the budget, so the
    // (correct) suppression left nothing to stream and the assertions below
    // failed on the test's own under-budgeting rather than on the engine.
    params.max_tokens = 500;
    params.temperature = 0.0f;

    StreamCapture cap;
    std::atomic<bool> cancel{false};
    auto on_token = [&cap](std::string_view tok) { cap.push(tok); };

    auto r = ctx.orchestrator->generate_streaming({u}, params, on_token,
                                                  cancel, tier_name);

    std::printf("\n===gh108 CONTROL plain-decode streaming===\n"
                "code=%d drafted=%d tokens_streamed=%d\n"
                "streamed=[%s]\ncontent =[%s]\n===\n",
                r.error_code, r.n_drafted, cap.token_count,
                cap.text.c_str(), r.content.c_str());

    REQUIRE(r.error_code == 0);
    REQUIRE(r.n_drafted == 0);      // plain decode, MTP definitively off
    REQUIRE(cap.token_count > 0);
    // Same assertions as the MTP case. Whether these pass or fail is the
    // whole point of this control.
    CHECK(cap.text.find("<|channel") == std::string::npos);
    CHECK(r.content.find("<|channel") == std::string::npos);
}

TEST_CASE("gh#108 (v2.10.3) CONTROL: non-streaming generate on the same tier",
          "[model][gh108][control]") {
    // Third axis. The two controls above show the live-stream leak is not an
    // MTP property. This one asks whether result.content leaks on the
    // NON-streaming path too — i.e. whether the buffered-content half is a
    // streaming defect at all, or a property of this config (no tools staged,
    // so common_chat_parse_reliable may be false and strip_thinking_channels
    // never runs).
    ModelTestContext ctx;
    auto tier_name = configure_mtp_tier(ctx);
    ctx.config.inference.speculative.enabled = false;
    ctx.config.inference.speculative.mtp = false;
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Count to five, one number per line.";

    entropic::GenerationParams params;
    // gh#108 (v2.10.3): 48 was too tight. Once the channel leak was fixed,
    // gemma4 was still INSIDE its reasoning block at the budget, so the
    // (correct) suppression left nothing to stream and the assertions below
    // failed on the test's own under-budgeting rather than on the engine.
    params.max_tokens = 500;
    params.temperature = 0.0f;

    auto r = ctx.orchestrator->generate({u}, params, tier_name);

    std::printf("\n===gh108 CONTROL non-streaming===\ncode=%d\n"
                "content =[%s]\n===\n",
                r.error_code, r.content.c_str());

    REQUIRE(r.error_code == 0);
    CHECK(r.content.find("<|channel") == std::string::npos);
}
