// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh100_q8_kv.cpp
 * @brief gh#100: validate q8_0 KV-cache (cache_type_k/v) on the Gemma-4 E2B-Q4
 *        NPC tier under flash_attn — does it ~halve per-context KV (→ ~2x more
 *        #98 batch slots) WITHOUT the activation breakage that pinned E4B-Q4 to
 *        f16 KV? Backend-direct GPU test. Stochastic-safe: asserts grammar
 *        conformance + a bounded verb set, NEVER byte-exact q8==f16 (q8 is
 *        lossy and changes logits). E4B-Q4 is the documented negative anchor.
 *
 * @version 2.8.0 (gh#100)
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace entropic;

namespace {

fs::path model_path(const std::string& name) {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) / ".entropic" / "models" / name : fs::path{};
}

// Allowed NPC verb set; grammar forces output INTO this set (stochastic-safe:
// assert membership, never byte-exact q8==f16 — q8 is lossy, changes logits).
const char* kVerbGbnf =
    "root ::= \"MOVE\" | \"WAIT\" | \"ATTACK\" | \"FLEE\" | \"TALK\"\n";

struct KvProbe { bool ok = false; size_t kv_bytes = 0; std::string out; int in_tok = 0; };

KvProbe run_probe(const fs::path& gguf, const char* k, const char* v,
                  bool flash) {
    KvProbe r;
    LlamaCppBackend backend;
    ModelConfig cfg;
    cfg.path = gguf;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 99;          // E2B-Q4 is ~3 GB — fits fully on 11 GB
    cfg.flash_attn = flash;
    cfg.cache_type_k = k;
    cfg.cache_type_v = v;
    if (!backend.load(cfg) || !backend.activate()) { return r; }

    Message sys; sys.role = "system";
    sys.content = "You are a terse NPC. Reply with exactly one action verb.";
    Message usr; usr.role = "user";
    usr.content = "A goblin blocks the path. Act.";
    GenerationParams p;
    p.temperature = 0.0f;
    p.max_tokens = 4;
    p.grammar = kVerbGbnf;

    auto res = backend.generate({sys, usr}, p);
    r.in_tok = backend.last_input_tokens();
    std::vector<uint8_t> buf;
    bool saved = backend.save_state(0, buf);   // per-seq KV blob
    r.kv_bytes = saved ? buf.size() : 0;
    r.out = res.content;
    backend.deactivate();
    backend.unload();
    r.ok = saved && r.kv_bytes > 0;
    return r;
}

bool is_valid_verb(const std::string& s) {
    for (const char* v : {"MOVE", "WAIT", "ATTACK", "FLEE", "TALK"}) {
        if (s.find(v) != std::string::npos) { return true; }
    }
    return false;
}

}  // namespace

// ── A + B: the win + grammar equivalence on E2B-Q4 + flash_attn ──
SCENARIO("gh#100: q8_0 KV halves per-seq bytes and stays valid on "
         "Gemma-4 E2B-Q4 + flash_attn", "[model][gh100]") {
  GIVEN("Gemma-4 E2B-Q4 (UD-Q4_K_XL) on disk") {
    fs::path gguf = model_path("gemma-4-E2B-it-UD-Q4_K_XL.gguf");
    if (!fs::is_regular_file(gguf)) {
        SKIP("E2B-Q4 GGUF not present: " + gguf.string());
    }

    WHEN("generating the same verb prompt under f16 KV then q8_0 KV (FA on)") {
      auto f16 = run_probe(gguf, "f16",  "f16",  /*flash=*/true);
      auto q8  = run_probe(gguf, "q8_0", "q8_0", /*flash=*/true);

      THEN("both produce a valid verb and q8 KV is ~halved, no garbage") {
        INFO("f16_kv=" << f16.kv_bytes << "B q8_kv=" << q8.kv_bytes
             << " in_tok=" << f16.in_tok
             << " f16_out=[" << f16.out << "] q8_out=[" << q8.out << "]");
        REQUIRE(f16.ok);
        REQUIRE(q8.ok);
        // (A) per-seq KV bytes ~halved. q8_0 K/V ≈ 1 byte+scale vs f16 2 bytes;
        // a small fixed header keeps it above exactly 0.5, so assert a band.
        // NOTE: band is calibrated to the first observed GPU measurement.
        CHECK(static_cast<double>(q8.kv_bytes) <
              0.65 * static_cast<double>(f16.kv_bytes));
        CHECK(static_cast<double>(q8.kv_bytes) >
              0.40 * static_cast<double>(f16.kv_bytes));  // not collapsed/empty
        // (no NaN/activation breakage) q8 decodes a non-empty in-set verb.
        REQUIRE_FALSE(q8.out.empty());
        // (B) equivalence: grammar conformance + bounded verb-set (NOT q8==f16).
        CHECK(is_valid_verb(f16.out));
        CHECK(is_valid_verb(q8.out));
      }
    }
  }
}

// ── D: floor — default f16 KV path on E2B-Q4 is unchanged (additive) ──
SCENARIO("gh#100 floor: default f16 KV path on E2B-Q4 is unchanged",
         "[model][gh100]") {
  GIVEN("Gemma-4 E2B-Q4 on disk") {
    fs::path gguf = model_path("gemma-4-E2B-it-UD-Q4_K_XL.gguf");
    if (!fs::is_regular_file(gguf)) { SKIP("E2B-Q4 GGUF not present"); }
    WHEN("running the default (f16) KV path with q8 OFF") {
      auto f16 = run_probe(gguf, "f16", "f16", /*flash=*/true);
      THEN("it produces a valid verb with a populated KV cache") {
        REQUIRE(f16.ok);
        CHECK(is_valid_verb(f16.out));
        CHECK(f16.kv_bytes > 0);
      }
    }
  }
}

// ── C: negative reference — E4B-Q4 is the known-breakage anchor ──
// Documents the arch/quant boundary. NON-FATAL on q8: if q8 breaks E4B-Q4
// (garbage/empty) we record it as the documented negative; if a future
// llama.cpp pin fixes it, the WARN can be upgraded to a CHECK.
SCENARIO("gh#100 negative ref: E4B-Q4 + q8 KV breakage boundary",
         "[model][gh100]") {
  GIVEN("Gemma-4 E4B-Q4 (UD-Q4_K_XL) on disk") {
    fs::path gguf = model_path("gemma-4-E4B-it-UD-Q4_K_XL.gguf");
    if (!fs::is_regular_file(gguf)) { SKIP("E4B-Q4 GGUF not present"); }
    WHEN("generating under f16 then q8 KV (FA on)") {
      auto f16 = run_probe(gguf, "f16",  "f16",  true);
      auto q8  = run_probe(gguf, "q8_0", "q8_0", true);
      THEN("f16 is valid; q8 result recorded as the documented boundary") {
        INFO("E4B f16_out=[" << f16.out << "] q8_out=[" << q8.out << "]");
        REQUIRE(f16.ok);
        CHECK(is_valid_verb(f16.out));
        // q8 on E4B-Q4 + FA is the KNOWN-breakage anchor (docs/ENTROPIC_NPC_
        // INFERENCE.md). Record it; do NOT FAIL the suite on a known-unsafe
        // pin. Flip this WARN to a CHECK if a future pin fixes it.
        if (!q8.ok || !is_valid_verb(q8.out)) {
            WARN("E4B-Q4 + q8 KV produced no valid verb (documented "
                 "boundary, expected at this pin): [" + q8.out + "]");
        } else {
            WARN("E4B-Q4 + q8 KV produced a valid verb — the breakage "
                 "boundary may have shifted at this pin: [" + q8.out + "]");
        }
      }
    }
  }
}
