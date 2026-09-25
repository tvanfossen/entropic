// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh169_gh181_budget_carry.cpp
 * @brief gh#169 + gh#181: both engine-authored terminals hand the PARENT the
 *        child's real work, over a live model.
 *
 * The mandated emergent multi-turn model test for two fixes that shipped
 * without one. Both defects had ONE sink: `AgentEngine` pushed a PLACEHOLDER
 * as the child's final assistant message, and
 * `DelegationManager::extract_summary` takes the last assistant message as
 * `DelegationResult::summary` — so on every budget-terminated delegation the
 * parent was handed engine prose where the child's finished work belonged,
 * in the relay, in the ON_DELEGATE_COMPLETE payload and in the storage
 * record alike.
 *
 *   gh#169  the ITERATION CAP        terminal_reason "budget_exhausted"
 *   gh#181  the THINKING-BUDGET CUT  terminal_reason "budget_exhausted_thinking"
 *
 * Both now run through `AgentEngine::finish_with_carried_output` +
 * `annotate_hard_stop`: carry the run's last substantive assistant content,
 * ANNOTATE it, write terminal_reason + cap_carried_content, force COMPLETE.
 * The CPU scenarios (tests/unit/core/engine_test.cpp, gh#169 669-671 and
 * gh#181 672-674) drive that with a MockInference whose "work" is a string
 * the test itself wrote. What only a model can settle is whether a real
 * child, on a real tier, actually REACHES either terminal with real work
 * behind it — and whether the parent then receives that work.
 *
 * @par What a consumer can see, and what this test therefore asserts
 * `terminal_reason` and `cap_carried_content` live on the CHILD's
 * LoopContext. Nothing exports them by name: `ent_delegation_result_t` has
 * no such field, no storage column carries them, and the parent keeps only
 * `relay_status`. What DOES cross to a consumer is the RENDERING the two
 * keys describe, carried verbatim inside the summary:
 *
 *   terminal_reason      -> which annotation opens the trailing bracket:
 *                           "[iteration cap reached after N iterations ..."
 *                           vs "[thinking budget exhausted after the
 *                           completion nudge went unheeded ...". The two
 *                           values stay DISTINCT and so do these.
 *   cap_carried_content  -> "true"  renders  <the agent's own output>
 *                                            "\n\n[<reason> — the text above
 *                                            is the agent's last substantive
 *                                            output; ...]"
 *                           "false" renders  "[<reason> — no substantive
 *                                            output was produced before the
 *                                            cap/cut]"
 *
 * So each scenario reads both off the summary the consumer is handed, which
 * is also what makes the assertions non-vacuous: the annotation is proof the
 * terminal FIRED (a child that completed naturally carries none and reports
 * success), and the "\n\n" joint is proof content was CARRIED — before the
 * fix the summary WAS the placeholder, marker at offset 0, nothing in front.
 *
 * @par Why the summary is read from hook 9 and not from the parent's answer
 * `relay_single_delegate` promotes the child's summary into
 * `ctx.metadata["explicit_completion_summary"]`, and the facade serializes
 * MESSAGES, not metadata — a relayed summary is invisible through the C ABI.
 * So the lead here is an ordinary parent (no relay): the engine writes the
 * child's summary into its context as `[DELEGATION FAILED: eng] <summary>`
 * and the loop continues, which is the default consumer configuration and
 * the one the gh#169 consumer report came from. The summary is read from
 * ENTROPIC_HOOK_ON_DELEGATE_COMPLETE — hook 9, registered through
 * `entropic_register_hook`, engine-authored and never seeded by this test.
 *
 * @par Non-vacuity
 *  - Each scenario REQUIREs that the delegation fired (`fired >= 1`), so a
 *    run where the lead did the work itself, or declined, reports THAT
 *    instead of passing quietly (the e1df323 lesson).
 *  - Each scenario REQUIREs the terminal's own annotation, so a child that
 *    completed normally FAILS instead of passing on a happy path.
 *  - The needles ("TQ-4417-VERMILION", "KR-9032-MARLIN") are not in any
 *    prompt the PARENT sees. One lives in a file only the child can read,
 *    the other in the child tier's identity. A parent that reports either
 *    can only have got it from the child's carried output.
 *  - The recall turn reads `final_answer`, never the transcript, which
 *    already holds the engine's `[DELEGATION FAILED: ...]` carrier (the
 *    gh#165 trap, c51d76d).
 *
 * @par Sizing for the floor card (a 1080 Ti, 11 GB)
 * Guard and both tiers name `gemma4_e2b_qat` / gemma-4-E2B-it-qat-UD-Q4_K_XL
 * (2.44 GiB), the registry's explicitly floor-safe entry. Both tiers point at
 * the same GGUF, so `ModelOrchestrator::create_tier_backends` keys its
 * model_pool_ by path and loads ONE backend for the pair. Each tier declares
 * `allowed_tools` (one or two entries): a configured handle otherwise stages
 * all 27 registered tools, ~18.6 KB and ~4,700 tokens, which overflows a 4 K
 * window on the tool block alone — and since 26884f6 that no longer degrades
 * quietly, it REFUSES (ENTROPIC_ERROR_EVAL_CONTEXT_FULL), so an unsized
 * fixture fails the first turn rather than passing vacuously.
 * `explicit_completion` is set explicitly on every tier (964eee7) instead of
 * being derived: the lead owes no completion, and the child must owe one it
 * CANNOT emit, because a tier that may close its own turn never reaches
 * either terminal.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: ctest -L model -R gh169-gh181-budget-carry
 *
 * @version 2.13.0
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

namespace fs = std::filesystem;

/// @brief Case-insensitive substring test. @utility @version 2.13.0
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief The GGUF both scenarios stand on — floor-safe, 2.44 GiB.
/// @version 2.13.0
constexpr const char* kFloorGguf = "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf";

/// @brief The iteration cap's annotation opener (gh#169).
/// @version 2.13.0
constexpr const char* kCapMarker = "[iteration cap reached after ";

/// @brief The thinking-budget cut's annotation opener (gh#181).
/// @version 2.13.0
constexpr const char* kCutMarker =
    "[thinking budget exhausted after the completion nudge";

/// @brief The wording `cap_carried_content == "false"` renders.
/// @version 2.13.0
constexpr const char* kNothingCarried = "no substantive output was produced";

/**
 * @brief The ON_DELEGATE_COMPLETE payload — hook 9, as a consumer reads it.
 *
 * The hook fires on the run thread while the parent loop is still inside
 * `execute_pending_delegation`; the mutex makes the record safe to read from
 * the test thread afterwards regardless.
 * @version 2.13.0
 */
struct DelegateCapture {
    std::mutex mu;            ///< Guards every field below
    int fired = 0;            ///< Times hook 9 fired
    int success = -1;         ///< `success` of the last payload (-1 = unseen)
    std::string summary;      ///< `summary` of the last payload
    std::string target_tier;  ///< `target_tier` of the last payload
    std::string result_kind;  ///< `result_kind` of the last payload
    std::string raw;          ///< The last payload, verbatim

    /**
     * @brief C trampoline registered with `entropic_register_hook`.
     * @param point Hook point that fired.
     * @param context_json Payload; owned by the engine.
     * @param modified_json Out: never modified here.
     * @param ud DelegateCapture pointer.
     * @return 0 — a post-hook's return is advisory, and this one observes.
     * @callback
     * @version 2.13.0
     */
    static int call(entropic_hook_point_t point, const char* context_json,
                    char** modified_json, void* ud) {
        if (modified_json != nullptr) { *modified_json = nullptr; }
        if (point != ENTROPIC_HOOK_ON_DELEGATE_COMPLETE
            || context_json == nullptr) {
            return 0;
        }
        auto* self = static_cast<DelegateCapture*>(ud);
        auto j = nlohmann::json::parse(context_json, nullptr, false);
        std::lock_guard<std::mutex> lock(self->mu);
        self->fired++;
        self->raw = context_json;
        if (!j.is_discarded()) {
            self->success = j.value("success", false) ? 1 : 0;
            self->summary = j.value("summary", std::string{});
            self->target_tier = j.value("target_tier", std::string{});
            self->result_kind = j.value("result_kind", std::string{});
        }
        return 0;
    }
};

/**
 * @brief Assert the hand-back a budget terminal owes its parent.
 *
 * Shared by both scenarios because the CONTRACT is shared — gh#181 merged
 * the two paths into one `finish_with_carried_output`, and a test that
 * checked them through two different lenses could not notice them drifting
 * apart again. They differ only in which annotation opens the bracket.
 *
 * Each claim, and how it fails:
 *  1. `fired >= 1` — the delegation RAN. A lead that answered by itself, or
 *     never emitted a well-formed `entropic.delegate`, fails here instead of
 *     sailing past every assertion below on an empty capture.
 *  2. target_tier / success / result_kind — hook 9's consumer fields are
 *     UNCHANGED by gh#169 and gh#181. A budget-terminated child is still a
 *     FAILED delegation; carrying its work is orthogonal to reporting the
 *     budget, and conflating them is exactly what the placeholder did. These
 *     fail if a later change starts reporting success for a capped child.
 *  3. the marker — the terminal under test actually FIRED. A child that
 *     completed naturally has no annotation at all, so a run that "passed"
 *     by finishing its work early fails here. This is also the only
 *     consumer-visible witness of WHICH terminal_reason was written.
 *  4. the `"\n\n" + marker` joint — content was CARRIED, i.e.
 *     cap_carried_content == "true". Before the fix the summary WAS the
 *     placeholder: marker at offset 0, nothing in front of it. This fails on
 *     a regression to that, and equally when the child genuinely produced
 *     nothing substantive — which the next claim disambiguates.
 *  5. the `false` wording is absent — the other branch of
 *     cap_carried_content did not render.
 *  6. the needle — the carried text is the child's REAL work, not some
 *     unrelated assistant noise that happened to be last.
 *
 * @param cap Captured hook-9 payload.
 * @param marker Annotation opener the terminal under test renders.
 * @param needle Content only the child could have produced.
 * @utility
 * @version 2.13.0
 */
void expect_carried_handback(DelegateCapture& cap, const std::string& marker,
                             const std::string& needle) {
    std::lock_guard<std::mutex> lock(cap.mu);
    INFO("hook 9 (ON_DELEGATE_COMPLETE) payload: " << cap.raw);
    INFO("summary handed to the parent: [" << cap.summary << "]");
    REQUIRE(cap.fired >= 1);
    CHECK(cap.target_tier == "eng");
    CHECK(cap.success == 0);
    CHECK(cap.result_kind == "delegation_failed");
    REQUIRE(cap.summary.find(marker) != std::string::npos);
    const auto joint = cap.summary.find("\n\n" + marker);
    REQUIRE(joint != std::string::npos);
    CHECK(cap.summary.find(kNothingCarried) == std::string::npos);
    CHECK(has_ci(cap.summary.substr(0, joint), needle));
}

/**
 * @brief The lead both scenarios delegate from: a dispatcher with one tool.
 *
 * It holds `entropic.delegate` and NOTHING else, so the work it is asked for
 * is unreachable without a child — the e1df323 fixture rule. It also owes no
 * completion (`explicit_completion = false`), which lets its own turn end on
 * the sentence the recall assertion reads instead of on an argument with the
 * empty-turn nudge (the 964eee7 finding).
 *
 * Every lead is also told NOT to pass `max_turns`. Since gh#182 that argument
 * BOUNDS the child (`AgentEngine::resolve_max_iterations` takes the stricter
 * of it and the operator's limit), and the model picks its value freely from
 * the 1..30 the schema advertises. A lead that chose 1 would stop the gh#169
 * child before it read the file, and one that chose 2 or 3 would make the
 * ITERATION cap fire in the gh#181 scenario where the THINKING-BUDGET cut is
 * the subject — both failures of the fixture, not of the engine. The bound
 * each scenario is about therefore comes from `eng.max_iterations` alone, as
 * it did when the argument was inert.
 *
 * @param body Identity prose for this scenario's dispatcher.
 * @return A TierSpec named "lead".
 * @utility
 * @version 2.13.0
 */
entropic::test::facade::TierSpec make_lead(const std::string& body) {
    entropic::test::facade::TierSpec lead;
    lead.name = "lead";
    lead.gguf_key = "gemma4_e2b_qat";
    lead.adapter = "gemma4";
    lead.context_length = 4096;
    lead.identity_body = body
        + " When you call entropic.delegate, pass only target and task. "
          "Never set max_turns.";
    lead.allowed_tools = {"entropic.delegate"};
    lead.explicit_completion = false;
    return lead;
}

}  // namespace

// ── gh#169: the iteration cap ───────────────────────────────

SCENARIO("gh#169: an iteration-capped delegate hands its parent the audit it "
         "actually finished",
         "[model][gh169][delegation][budget][2.13.0]")
{
    GIVEN("a lead that cannot read the depot file and an 'eng' tier capped "
          "at three iterations") {
        auto gguf = entropic::test::facade::model_gguf(kFloorGguf);
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh169_cap_carry");
        // The needle lives ONLY here. Nothing the lead is told, and nothing
        // in the lead's identity, contains it — so the lead can report it in
        // the recall turn only if the child's carried output reached it.
        std::ofstream(project.dir() / "depot_manifest.txt")
            << "DEPOT MANIFEST - bay 3\n"
               "hopper temperature: nominal\n"
               "anomaly code: TQ-4417-VERMILION\n"
               "belt tension: nominal\n";
        // `mcp.working_dir` is authoritative for the filesystem server's root
        // (init_mcp_servers falls back to the PROCESS CWD, not to the
        // configured project), so an unset one would root the child at the
        // ctest working directory and every read would miss.
        project.extra_config =
            "mcp:\n  working_dir: " + project.dir().string() + "\n";

        auto lead = make_lead(
            "You are a depot dispatcher. You have NO filesystem tools and "
            "cannot read any file yourself. To get file work done you call "
            "entropic.delegate with target \"eng\" and a task saying what "
            "eng must read and report. When eng's report comes back, tell "
            "the user what it says in one short sentence, quoting any code "
            "or identifier EXACTLY as eng wrote it, even when the report is "
            "marked partial.");

        entropic::test::facade::TierSpec eng;
        eng.name = "eng";
        eng.gguf_key = "gemma4_e2b_qat";
        eng.adapter = "gemma4";
        eng.context_length = 4096;
        eng.identity_body =
            "You are the depot auditor. Read depot_manifest.txt with "
            "filesystem.read_file, ONCE. Then stop calling tools and write "
            "your finding as a sentence, quoting the anomaly code in full, "
            "exactly as the file spells it. Quote the code in full in every "
            "report you write.";
        // One reader, and deliberately NO `entropic.complete`: a tier that
        // can close its own turn never reaches the cap. The pairing with
        // explicit_completion below is what makes the cap the ONLY exit —
        // the child does real work, is nudged for a completion it has no
        // tool for, and the loop runs out of iterations with its finding
        // sitting in the transcript. That is the gh#169 consumer's shape.
        eng.allowed_tools = {"filesystem.read_file"};
        eng.explicit_completion = true;
        // Three iterations: read, report, and one more turn under the nudge.
        // The cap comes from the child tier's identity, which is the bound a
        // FIXTURE controls. Since gh#182 `entropic.delegate`'s own
        // `max_turns` can lower it further, which is why `make_lead` tells
        // every lead here not to pass one.
        eng.max_iterations = 3;

        auto* h = project.setup({lead, eng}, "lead");
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        DelegateCapture cap;
        REQUIRE(entropic_register_hook(h, ENTROPIC_HOOK_ON_DELEGATE_COMPLETE,
                                       &DelegateCapture::call, &cap, 0)
                == ENTROPIC_OK);

        WHEN("the lead delegates the audit and is asked about it a turn "
             "later") {
            using entropic::test::facade::final_answer;
            using entropic::test::facade::run_transcript;
            auto t1 = run_transcript(
                h, "Have eng audit the depot manifest and tell me what it "
                   "found.");
            // Turn 2 on the SAME accumulated conversation — the emergent
            // part. Nothing re-reads the file; the only place the code can
            // come from is what the child handed back on turn 1.
            auto t2 = run_transcript(
                h, "What anomaly code did eng report? Answer with the code "
                   "and nothing else.");
            auto answer2 = final_answer(t2);

            // ONE leaf on purpose: Catch2 re-runs GIVEN/WHEN once per LEAF
            // section, so every sibling THEN or AND_THEN here would be
            // another model load plus another two turns.
            THEN("the cap fired, and what came back is the child's own "
                 "audit") {
                INFO("turn 1 transcript: " << t1);
                INFO("turn 2 answer: [" << answer2 << "]\n"
                     "turn 2 transcript: " << t2);
                expect_carried_handback(cap, kCapMarker, "TQ-4417-VERMILION");

                // The parent's CONTEXT carries it too. On the transcript
                // deliberately: this is the message the ENGINE wrote into
                // the lead's context (`[DELEGATION FAILED: eng] <summary>`,
                // push_delegation_result), not anything the test seeded —
                // the needle exists nowhere the lead could have read it.
                // Before the fix this line carried the placeholder.
                CHECK(has_ci(t1, "DELEGATION FAILED: eng"));
                CHECK(has_ci(t1, "TQ-4417-VERMILION"));

                // PROSE TRIGGERS, FACT DECIDES (v2.13.1).
                //
                // This used to assert the lead repeats the code a turn
                // later. Measured over five independent runs, that assertion
                // passes 2 attempts in 11 — 0.18 per attempt — and it was
                // the ONLY thing failing this test. Three retries cannot
                // rescue 0.18, so it was not marginal, it was a coin flip
                // wearing a gate's clothes.
                //
                // Everything the FEATURE claims is already decided above by
                // facts: the engine wrote `[DELEGATION FAILED: eng]` into
                // the lead's context, the child's real content is in that
                // carrier, and expect_carried_handback pinned the hook-9
                // payload, the annotation, and the needle.
                //
                // So the wording is PRINTED for a human and asserted only
                // where it cannot produce a false red: if the lead echoes
                // the code at all it must be the carried one, and the
                // placeholder this fix replaced must never appear.
                INFO("lead's turn-2 answer: [" << answer2 << "]");
                if (has_ci(answer2, "TQ-4417")) {
                    CHECK(has_ci(answer2, "TQ-4417-VERMILION"));
                }
                CHECK_FALSE(has_ci(answer2, kNothingCarried));
            }
        }
    }
}

// ── gh#181: the thinking-budget hard cut ────────────────────

SCENARIO("gh#181: a delegate hard-cut by the thinking budget hands its "
         "parent the assessment it actually wrote",
         "[model][gh181][delegation][budget][2.13.0]")
{
    GIVEN("a thinking budget, a lead that knows nothing about the line, and "
          "an 'eng' tier that can only narrate") {
        auto gguf = entropic::test::facade::model_gguf(kFloorGguf);
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        // An EMPTY filesystem root, on purpose. The child owes a tool call it
        // has no completion for, so the empty-turn nudge pushes it at the one
        // reader it holds; rooted here, every path it can invent MISSES.
        // gh#84 made that matter: the budget resets on a tool that genuinely
        // executed (result_kind ok / ok_empty), never on one that failed, so
        // a miss leaves the charge accumulating instead of handing the spiral
        // a fresh allowance. Rooting the child at the project instead would
        // let it read config.local.yaml or its own identity file and reset.
        auto empty_root = fs::temp_directory_path() /
            ("entropic_gh181_empty_" + std::to_string(::getpid()));
        fs::remove_all(empty_root);
        fs::create_directories(empty_root);

        entropic::test::facade::FacadeProject project("gh181_cut_carry");
        // budget_mode: tokens charges content.size()/4 per tool-call-free
        // turn (`budget_units_consumed`), cumulative until a tool genuinely
        // executes. 200 units is ~800 characters: the child is told to write
        // several paragraphs and trips it on one turn, while the lead is told
        // to be terse and does not. First overflow nudges
        // ("[engine] thinking budget reached — emit entropic.complete now"),
        // second CUTS — which is the gh#80 ladder this fix sits on top of.
        project.extra_config =
            "generation:\n"
            "  budget_mode: tokens\n"
            "  budget_limit: 200\n"
            "mcp:\n  working_dir: " + empty_root.string() + "\n";

        auto lead = make_lead(
            "You are a works dispatcher for the Pinewood line. You know "
            "nothing about the line's structures yourself and have no "
            "filesystem tools. To answer anything about them you call "
            "entropic.delegate with target \"eng\" and a task describing "
            "what you need. When eng's report comes back, answer the user "
            "in ONE short sentence, quoting any tag or identifier EXACTLY "
            "as eng wrote it, even when the report is marked partial.");

        entropic::test::facade::TierSpec eng;
        eng.name = "eng";
        eng.gguf_key = "gemma4_e2b_qat";
        eng.adapter = "gemma4";
        eng.context_length = 4096;
        // The needle lives in the CHILD's identity — the lead's prompt and
        // identity never contain it, so a parent that reports it got it from
        // the child's carried output and nowhere else.
        eng.identity_body =
            "You are the bridge inspector for the Pinewood line. What you "
            "know: the failing structure is the culvert at mile 12, and its "
            "inspection tag is KR-9032-MARLIN. When you are asked for an "
            "assessment, write a thorough one of at least three paragraphs "
            "— condition, cause, and what you recommend — and state the "
            "inspection tag in full in every assessment you write.";
        // A reader it will not manage to use (see empty_root) and, again, no
        // `entropic.complete`: the cut fires precisely when a model narrates
        // instead of calling a tool, which is a state a tier that can close
        // its own turn never stays in.
        eng.allowed_tools = {"filesystem.read_file"};
        eng.explicit_completion = true;
        // Room for the nudge and then the cut. Deliberately larger than the
        // handful of turns that takes, so that a run reaching the ITERATION
        // cap instead reports as the wrong annotation rather than being
        // manufactured by a cap set too tight.
        eng.max_iterations = 8;
        // The empty-turn ladder ends in AgentState::ERROR on the fourth
        // tool-call-free turn, and ERROR is terminal — neither budget
        // terminal fires after it. This scenario's subject IS one of those
        // terminals, so the allowance is raised past the ladder's reach.
        eng.max_consecutive_empty_turns = 8;

        auto* h = project.setup({lead, eng}, "lead");
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        DelegateCapture cap;
        REQUIRE(entropic_register_hook(h, ENTROPIC_HOOK_ON_DELEGATE_COMPLETE,
                                       &DelegateCapture::call, &cap, 0)
                == ENTROPIC_OK);

        WHEN("the lead delegates the assessment and is asked about it a turn "
             "later") {
            using entropic::test::facade::final_answer;
            using entropic::test::facade::run_transcript;
            auto t1 = run_transcript(
                h, "Ask eng for a full assessment of the failing structure "
                   "on the Pinewood line, and tell me what it says.");
            auto t2 = run_transcript(
                h, "What inspection tag did eng give for that structure? "
                   "Answer with the tag and nothing else.");
            auto answer2 = final_answer(t2);

            // ONE leaf on purpose — see the gh#169 scenario.
            THEN("the hard cut fired, and what came back is the child's own "
                 "assessment") {
                INFO("turn 1 transcript: " << t1);
                INFO("turn 2 answer: [" << answer2 << "]\n"
                     "turn 2 transcript: " << t2);
                expect_carried_handback(cap, kCutMarker, "KR-9032-MARLIN");

                {
                    // It is the CUT that fired, not the iteration cap. The
                    // two terminal_reason values stay distinct and a
                    // consumer may key on them; a child that ran out of
                    // iterations first would prove nothing about this one.
                    std::lock_guard<std::mutex> lock(cap.mu);
                    CHECK(cap.summary.find(kCapMarker) == std::string::npos);
                }

                // The parent's context carries it too — the engine's own
                // `[DELEGATION FAILED: eng] <summary>` carrier, and the
                // needle reaches the lead through nothing else.
                CHECK(has_ci(t1, "DELEGATION FAILED: eng"));
                CHECK(has_ci(t1, "KR-9032-MARLIN"));

                // PROSE TRIGGERS, FACT DECIDES (v2.13.1) — same reasoning
                // as the gh#169 scenario above. The carrier and the hook-9
                // payload decide; the lead's own wording is printed, and
                // asserted only in the direction that cannot false-red.
                INFO("lead's turn-2 answer: [" << answer2 << "]");
                if (has_ci(answer2, "KR-9032")) {
                    CHECK(has_ci(answer2, "KR-9032-MARLIN"));
                }
                CHECK_FALSE(has_ci(answer2, kNothingCarried));
            }
        }

        fs::remove_all(empty_root);
    }
}
