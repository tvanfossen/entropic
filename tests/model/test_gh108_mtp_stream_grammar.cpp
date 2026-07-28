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

TEST_CASE("gh#134 (v2.10.4) require_tool_call forces a tool call under MTP",
          "[model][gh134][mtp][grammar][toolchoice]") {
    // The two unknowns this measures, both unanswerable before P2+P3 landed
    // because the render's grammar was discarded and REQUIRED was inert:
    //
    //  (a) does grammar_lazy=false forbid gemma4's thinking channel? If it
    //      does, this knob and enable_thinking are mutually exclusive — which
    //      is bissell-explorer's exact config (gh#134).
    //  (b) does MTP accept-rate collapse when the head drafts tokens an eager
    //      grammar rejects? If so MTP+require_tool_call is a slowdown, not a
    //      composition.
    ModelTestContext ctx;
    auto tier_name = configure_mtp_tier(ctx);
    ctx.config.models.tiers[tier_name].require_tool_call = true;  // gh#134
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "What is the weather in Paris?";

    entropic::GenerationParams params;
    params.max_tokens = 500;
    params.temperature = 0.0f;
    params.enable_thinking = true;   // unknown (a): does eager grammar allow it
    params.tools =
        R"JSON([{"name":"get_weather","description":"Get weather for a city",
        "inputSchema":{"type":"object","properties":{"location":
        {"type":"string"}},"required":["location"]}}])JSON";

    StreamCapture cap;
    std::atomic<bool> cancel{false};
    auto on_token = [&cap](std::string_view tok) { cap.push(tok); };

    auto r = ctx.orchestrator->generate_streaming({u}, params, on_token,
                                                  cancel, tier_name);

    std::printf("\n===gh134 require_tool_call + MTP===\n"
                "code=%d drafted=%d accepted=%d calls=%zu\n"
                "content=[%s]\n===\n",
                r.error_code, r.n_drafted, r.n_accepted, r.tool_calls.size(),
                r.content.c_str());

    REQUIRE(r.error_code == 0);

    // THE POINT: narrate-then-stop must be unrepresentable. gh#134 measured
    // 0/12 recovery past the third empty-turn nudge, so a post-hoc nudge is
    // not a substitute for structural enforcement.
    CHECK(r.tool_calls.size() >= 1);

    // (b) MTP still engaged rather than being bypassed or starved.
    CHECK(r.n_drafted > 0);
}

TEST_CASE("gh#134 (v2.10.4) A/B: the knob, not the model, produces the call",
          "[model][gh134][mtp][grammar][toolchoice][ab]") {
    // The control the single-case test above lacks.
    //
    // `calls >= 1` with require_tool_call=true proves nothing on its own: the
    // model may emit that call regardless, and bissell's narrate-then-stop is
    // INTERMITTENT (~23 occurrences, onset after ~5 successful calls). So a
    // passing test could simply be travelling the unchanged path — exactly the
    // failure mode that let v2.10.0 ship an unproven claim.
    //
    // This runs the SAME tier and SAME prompt twice, flipping only the knob,
    // and prints both. Two things it establishes:
    //
    //   1. require_tool_call=false still works (default-off regression guard —
    //      every existing consumer rides this path).
    //   2. Whether the OFF arm can ever produce 0 calls. If OFF also always
    //      yields a call, this prompt cannot discriminate and the assertion
    //      below is knowingly weak — the printed counts say so rather than a
    //      green tick implying more than was measured.
    //
    // A prompt that RELIABLY induces narrate-then-stop would be strictly
    // better. bissell's signature needs ~5 prior tool calls to onset, which a
    // single turn cannot reproduce; that is the multi-turn test, not this one.

    auto run_once = [](bool require_tool_call, int& out_calls,
                       int& out_drafted, std::string& out_content) {
        ModelTestContext ctx;
        auto tier_name = configure_mtp_tier(ctx);
        ctx.config.models.tiers[tier_name].require_tool_call = require_tool_call;
        if (!init_orchestrator(ctx)) {
            SKIP("orchestrator init failed (resource/config) — A/B untested");
        }

        entropic::Message u;
        u.role = "user";
        u.content = "What is the weather in Paris?";

        entropic::GenerationParams params;
        params.max_tokens = 500;
        params.temperature = 0.0f;
        params.enable_thinking = true;
        params.tools =
            R"JSON([{"name":"get_weather","description":"Get weather for a city",
            "inputSchema":{"type":"object","properties":{"location":
            {"type":"string"}},"required":["location"]}}])JSON";

        StreamCapture cap;
        std::atomic<bool> cancel{false};
        auto on_token = [&cap](std::string_view tok) { cap.push(tok); };
        auto r = ctx.orchestrator->generate_streaming({u}, params, on_token,
                                                      cancel, tier_name);
        REQUIRE(r.error_code == 0);
        out_calls = static_cast<int>(r.tool_calls.size());
        out_drafted = r.n_drafted;
        out_content = r.content;
    };

    int off_calls = 0, off_drafted = 0, on_calls = 0, on_drafted = 0;
    std::string off_content, on_content;

    run_once(false, off_calls, off_drafted, off_content);
    run_once(true, on_calls, on_drafted, on_content);

    std::printf("\n===gh134 A/B require_tool_call===\n"
                "OFF: calls=%d drafted=%d content=[%s]\n"
                "ON : calls=%d drafted=%d content=[%s]\n"
                "discriminating=%s\n===\n",
                off_calls, off_drafted, off_content.c_str(),
                on_calls, on_drafted, on_content.c_str(),
                (off_calls == 0 && on_calls >= 1) ? "YES" : "NO (see note)");

    // Default-off must keep working — the path every existing consumer takes.
    CHECK(off_drafted > 0);

    // The knob's contract.
    CHECK(on_calls >= 1);
}

TEST_CASE("gh#134 (v2.10.4) multi-turn: rails hold past the stall onset",
          "[model][gh134][mtp][grammar][toolchoice][multiturn]") {
    // The A/B above proved this feature's single-turn test was VACUOUS: with
    // the knob off the model called the tool anyway, so `calls>=1` measured
    // the model's disposition, not the grammar.
    //
    // bissell's narrate-then-stop has a specific signature: the tier makes
    // ~5 successful tool calls, THEN begins ending turns with prose and zero
    // calls until the empty-turn allowance exhausts (0/12 recovery past the
    // third nudge). A single easy question cannot reach that state, which is
    // why no single-turn test can discriminate here.
    //
    // So: drive a scripted sequence long enough to pass the reported onset,
    // with tools staged every turn, and count prose-only turns on BOTH arms.
    // The knob earns its keep only if the OFF arm stalls somewhere the ON arm
    // does not. If NEITHER stalls, this box simply does not reproduce their
    // failure and the printed counts say so — a green tick here must not be
    // read as "narrate-then-stop is fixed".
    constexpr int kTurns = 8;

    auto run_sequence = [](bool require_tool_call, int& out_prose_only,
                           int& out_calls, std::string& out_trace) {
        ModelTestContext ctx;
        auto tier_name = configure_mtp_tier(ctx);
        ctx.config.models.tiers[tier_name].require_tool_call = require_tool_call;
        if (!init_orchestrator(ctx)) {
            SKIP("orchestrator init failed (resource/config) — untested");
        }

        entropic::GenerationParams params;
        params.max_tokens = 400;
        params.temperature = 0.0f;
        params.enable_thinking = true;
        params.tools =
            R"JSON([{"name":"get_weather","description":"Get current weather for a city",
            "inputSchema":{"type":"object","properties":{"location":
            {"type":"string"}},"required":["location"]}},
            {"name":"search_notes","description":"Search the user's saved notes",
            "inputSchema":{"type":"object","properties":{"query":
            {"type":"string"}},"required":["query"]}}])JSON";

        // Accumulating conversation — the state that produces the onset.
        std::vector<entropic::Message> conv;
        const char* asks[kTurns] = {
            "What is the weather in Paris?",
            "Now Tokyo.",
            "And London.",
            "Search my notes for 'Q3 budget'.",
            "Now check the weather in Berlin.",
            "Search my notes for 'roadmap'.",
            "Weather in Madrid please.",
            "Search my notes for 'hiring'.",
        };

        for (int t = 0; t < kTurns; ++t) {
            entropic::Message u;
            u.role = "user";
            u.content = asks[t];
            conv.push_back(u);

            std::atomic<bool> cancel{false};
            std::function<void(std::string_view)> on_token =
                [](std::string_view) {};
            auto r = ctx.orchestrator->generate_streaming(conv, params,
                                                          on_token, cancel,
                                                          tier_name);
            if (r.error_code != 0) {
                out_trace += "E";
                break;
            }
            bool called = !r.tool_calls.empty();
            out_calls += static_cast<int>(r.tool_calls.size());
            // A prose-only turn IS the reported failure: content, no call.
            if (!called) { ++out_prose_only; }
            out_trace += called ? "C" : "p";

            entropic::Message a;
            a.role = "assistant";
            a.content = r.content;
            conv.push_back(a);

            if (called) {  // feed a canned result so the loop can continue
                entropic::Message tr;
                tr.role = "user";
                tr.content = (r.tool_calls[0].name == "get_weather")
                    ? "18C, partly cloudy" : "Q3: increase spend 12%.";
                tr.metadata["tool_call_id"] = r.tool_calls[0].id;
                tr.metadata["tool_name"] = r.tool_calls[0].name;
                conv.push_back(tr);
            }
        }
    };

    int off_prose = 0, off_calls = 0, on_prose = 0, on_calls = 0;
    std::string off_trace, on_trace;

    run_sequence(false, off_prose, off_calls, off_trace);
    run_sequence(true, on_prose, on_calls, on_trace);

    std::printf("\n===gh134 MULTI-TURN A/B (%d turns, C=call p=prose-only)===\n"
                "OFF: trace=%s prose_only=%d calls=%d\n"
                "ON : trace=%s prose_only=%d calls=%d\n"
                "reproduced_stall_with_knob_off=%s\n===\n",
                kTurns, off_trace.c_str(), off_prose, off_calls,
                on_trace.c_str(), on_prose, on_calls,
                off_prose > 0 ? "YES" : "NO — box did not reproduce");

    // The knob's contract, and the only assertion that can be made honestly:
    // with rails on, no turn may end in prose.
    // A sequence that never ran must FAIL rather than pass on a vacuous
    // `on_prose == 0`. An earlier rerun broke on turn 1 and went green with
    // ZERO turns executed — the same vacuous-pass shape this whole
    // investigation exists to eliminate.
    REQUIRE(off_calls + off_prose > 0);
    REQUIRE(on_calls + on_prose > 0);

    CHECK(on_prose == 0);
}

TEST_CASE("gh#134 (v2.10.4) closest replication of the reported stall config",
          "[model][gh134][mtp][grammar][toolchoice][repro]") {
    // Fourth and final reproduction attempt. Three prior tests were all
    // non-discriminating (the OFF arm called the tool every time), so nothing
    // yet shows the knob prevents narrate-then-stop.
    //
    // This matches bissell-explorer's reported config as closely as this box
    // allows: gemma-4 E4B QAT (not E2B Q8), explicit_completion, a wide tool
    // surface including entropic.complete, and prompts that require actual
    // reasoning rather than being trivially tool-shaped. Their signature is
    // onset AFTER ~5 successful calls, so the sequence runs past that.
    //
    // If this ALSO fails to reproduce, the honest conclusion is that the
    // failure is not reachable on this hardware/config and only the consumer
    // can verify the fix. The printed reproduced= line records which it was.
    auto target = models_dir() / "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";
    auto head = models_dir() / "mtp-gemma-4-E4B-it.gguf";
    if (!std::filesystem::is_regular_file(target)) {
        SKIP("E4B QAT GGUF not present");
    }

    constexpr int kTurns = 10;

    auto run_sequence = [&](bool require_tool_call, int& out_prose,
                            int& out_calls, std::string& out_trace,
                            int& out_illegal_stop) {
        ModelTestContext ctx;
        REQUIRE(load_registry(ctx.registry));
        REQUIRE(load_test_config(ctx.registry, ctx.config));
        auto tier_name = ctx.config.models.default_tier;
        auto it = ctx.config.models.tiers.find(tier_name);
        if (it == ctx.config.models.tiers.end()) { SKIP("no default tier"); }
        auto& tier = it->second;
        tier.path = target;
        tier.adapter = "gemma4";
        tier.gpu_layers = 99;
        tier.context_length = 8192;   // longer context, closer to theirs
        tier.flash_attn = false;
        tier.cache_type_k = "f16";
        tier.cache_type_v = "f16";
        tier.grammar.reset();
        tier.require_tool_call = require_tool_call;
        // Their tier is mandatory-completion, one call per turn.
        tier.auto_chain.reset();      // => explicit_completion true (get_param)
        auto& spec = ctx.config.inference.speculative;
        if (std::filesystem::is_regular_file(head)) {
            spec.enabled = true;
            spec.mtp = true;
            spec.draft.path = head;
            spec.n_draft = 16;
        }
        if (!init_orchestrator(ctx)) { SKIP("orchestrator init failed"); }

        entropic::GenerationParams params;
        // gh#134: successful turns measured 186-519 generated tokens against
        // the old 600 cap — almost no headroom. Raised to 2000 to separate two
        // explanations of the ON-arm `length` stall that 600 could not:
        //   converges  -> forbidding the early prose exit merely needed more
        //                 room; budget is the fix.
        //   diverges   -> the grammar and the model are fighting and it never
        //                 reaches a legal call; more budget is NOT the fix and
        //                 the feature is unsafe to ship as a recommendation.
        params.max_tokens = 2000;
        params.temperature = 0.0f;
        params.enable_thinking = true;
        // Wider surface, including the completion tool their tiers must call.
        params.tools =
            R"JSON([{"name":"search_code","description":"Search the codebase for a symbol or pattern",
            "inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}},
            {"name":"read_file","description":"Read a file from the repository",
            "inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},
            {"name":"list_directory","description":"List a directory",
            "inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},
            {"name":"search_notes","description":"Search saved engineering notes",
            "inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}},
            {"name":"grep_symbol","description":"Find definitions of a symbol",
            "inputSchema":{"type":"object","properties":{"symbol":{"type":"string"}},"required":["symbol"]}},
            {"name":"entropic.complete","description":"Finish the turn with a summary",
            "inputSchema":{"type":"object","properties":{"summary":{"type":"string"}},"required":["summary"]}}])JSON";

        // Reasoning-heavy asks, in their idiom — the model must decide WHICH
        // tool and WHAT argument, rather than being handed an obvious call.
        const char* asks[kTurns] = {
            "Find every function that takes a play_sound_info_t parameter.",
            "Which of those are declared in a header rather than a .c file?",
            "Read the header that declares the most of them.",
            "What other audio types does that header expose?",
            "Search our notes for prior decisions about the audio mixer.",
            "Given those notes, which function would you change first?",
            "Show me the directory that function lives in.",
            "Are there sibling modules with the same pattern?",
            "Summarise what you have found about play_sound_info_t.",
            "Now record that summary as the turn's completion.",
        };

        std::vector<entropic::Message> conv;
        for (int t = 0; t < kTurns; ++t) {
            entropic::Message u;
            u.role = "user";
            u.content = asks[t];
            conv.push_back(u);

            std::atomic<bool> cancel{false};
            std::function<void(std::string_view)> on_token =
                [](std::string_view) {};
            auto r = ctx.orchestrator->generate_streaming(conv, params,
                                                          on_token, cancel,
                                                          tier_name);
            if (r.error_code != 0) {
                out_trace += "E(" + std::to_string(r.error_code) + ":"
                           + r.error_message + ")";
                break;
            }
            bool called = !r.tool_calls.empty();
            out_calls += static_cast<int>(r.tool_calls.size());
            if (!called) {
                ++out_prose;
                // A `stop` with zero calls is the contract violation: the
                // model ENDED legally without calling a tool, which
                // tool_choice=REQUIRED forbids. A `length` stall is a budget
                // misconfiguration, not a grammar failure — counted apart so
                // the two are never conflated again.
                if (r.finish_reason == "stop") { ++out_illegal_stop; }
                // THE decisive field. Under tool_choice=REQUIRED the gemma4
                // grammar is `zero_or_more(any) + tool_call` — unbounded
                // preamble, then a MANDATORY call. So a prose-only turn can
                // occur without violating the grammar if the token budget is
                // exhausted mid-preamble. finish_reason separates the two:
                //   "length" -> budget ran out before the mandated call
                //               (grammar fine; this is a budget/preamble bound)
                //   "stop"   -> model ENDED legally without a call, meaning
                //               the grammar was not applied -> wiring hole
                out_trace += "[" + r.finish_reason + "]";
            }
            out_trace += called ? "C" : "p";

            entropic::Message a;
            a.role = "assistant";
            a.content = r.content;
            conv.push_back(a);
            if (called) {
                entropic::Message tr;
                tr.role = "user";
                tr.content = "audio/mixer.h:42: void play_sound(play_sound_info_t*)";
                tr.metadata["tool_call_id"] = r.tool_calls[0].id;
                tr.metadata["tool_name"] = r.tool_calls[0].name;
                conv.push_back(tr);
            }
        }
    };

    int off_prose = 0, off_calls = 0, on_prose = 0, on_calls = 0;
    int off_illegal = 0, on_illegal = 0;
    std::string off_trace, on_trace;
    run_sequence(false, off_prose, off_calls, off_trace, off_illegal);
    run_sequence(true, on_prose, on_calls, on_trace, on_illegal);

    std::printf("\n===gh134 CLOSEST REPRO (E4B QAT, %d turns, C=call p=prose)===\n"
                "OFF: trace=%s prose_only=%d calls=%d\n"
                "ON : trace=%s prose_only=%d calls=%d\n"
                "illegal_stop: OFF=%d ON=%d\n"
                "reproduced=%s  knob_discriminates=%s\n===\n",
                kTurns, off_trace.c_str(), off_prose, off_calls,
                on_trace.c_str(), on_prose, on_calls,
                off_illegal, on_illegal,
                off_illegal > 0 ? "YES" : "NO",
                (off_illegal > 0 && on_illegal == 0) ? "YES" : "NO");

    // A sequence that never ran must FAIL rather than pass on a vacuous
    // `on_prose == 0`. An earlier rerun broke on turn 1 and went green with
    // ZERO turns executed — the same vacuous-pass shape this whole
    // investigation exists to eliminate.
    REQUIRE(off_calls + off_prose > 0);
    REQUIRE(on_calls + on_prose > 0);

    // The control: without the knob the model MUST be seen quitting legally,
    // or this run never exercised the failure and proves nothing about the fix.
    CHECK(off_illegal > 0);

    // THE CONTRACT: with the knob on, no turn may end legally without a call.
    // Asserted on finish_reason rather than a prose count — the earlier metric
    // compared prose counts (1 vs 1), reported "no effect", and hid that the
    // stall mechanism had changed entirely from stop to length.
    CHECK(on_illegal == 0);
}
