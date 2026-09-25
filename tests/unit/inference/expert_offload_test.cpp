// SPDX-License-Identifier: Apache-2.0
/**
 * @file expert_offload_test.cpp
 * @brief gh#153 #42(iii) (v2.13.0): expert-tensor offload, asserted on CPU.
 *
 * Three properties, none of which needs a GPU or a GGUF:
 *
 *  1. **The patterns are upstream's.** The tail regex is compared against the
 *     vendored `LLM_FFN_EXPS_REGEX` itself, and the assembled per-layer
 *     patterns are run with `std::regex_search` — the same function
 *     `llama-model-loader.cpp` uses — against the literal tensor names the
 *     `gemma4` MoE arch creates at this pin (`src/models/gemma4.cpp` +
 *     `llama-arch.cpp`'s LLM_TENSOR_NAMES). A pin bump that renames either
 *     side fails here rather than silently offloading nothing.
 *  2. **The array is a valid C contract.** Null-terminated, and every
 *     `pattern` points into storage the object owns for its whole life.
 *  3. **Absent means absent.** `cpu_moe_layers` at its default installs no
 *     overrides at all, which is the byte-identical-default guarantee.
 *
 * @version 2.13.0
 */

#include "../../../src/inference/expert_offload.h"
#include "../../../src/inference/expert_offload_buft.h"

#include <catch2/catch_test_macros.hpp>

// The vendored constant this header mirrors. Header-only constant; no link
// against llama.cpp's `common` library is required to read it.
#include <common.h>

#include <regex>
#include <string>
#include <vector>

using entropic::ExpertOffloadOverrides;
using entropic::expert_block_pattern;
using entropic::expert_offload_conflict_reason;
using entropic::expert_offload_load_refusal;
using entropic::expert_offload_patterns;
using entropic::kExpertTensorRegex;
using entropic::ModelConfig;

namespace {

/// @brief What llama-model-loader.cpp does per tensor, per pattern.
bool matches(const std::string& pattern, const std::string& tensor_name) {
    return std::regex_search(tensor_name, std::regex(pattern));
}

/// @brief Tensor names the gemma4 MoE arch actually creates for a layer.
///        `ffn_gate_up_exps` is the fused variant; `ffn_{gate,up}_exps` the
///        unfused pair; `.scale` the per-expert scale siblings that
///        `llama-model.cpp` attaches to each.
std::vector<std::string> gemma4_expert_tensors(int layer) {
    const std::string blk = "blk." + std::to_string(layer) + ".";
    return {
        blk + "ffn_gate_exps.weight",
        blk + "ffn_up_exps.weight",
        blk + "ffn_down_exps.weight",
        blk + "ffn_gate_up_exps.weight",
        blk + "ffn_gate_exps.scale",
        blk + "ffn_down_exps.scale",
        blk + "ffn_up_exps.scale",
    };
}

/// @brief Everything in a gemma4 MoE layer that must STAY where gpu_layers
///        put it — attention and its KV above all, plus the router, the
///        shared/dense FFN, the norms and the model-level tensors.
std::vector<std::string> gemma4_resident_tensors(int layer) {
    const std::string blk = "blk." + std::to_string(layer) + ".";
    return {
        blk + "attn_q.weight",         blk + "attn_k.weight",
        blk + "attn_v.weight",         blk + "attn_output.weight",
        blk + "attn_q_norm.weight",    blk + "attn_k_norm.weight",
        blk + "attn_norm.weight",      blk + "post_attention_norm.weight",
        blk + "ffn_gate_inp.weight",   blk + "ffn_gate_inp.scale",
        blk + "ffn_gate.weight",       blk + "ffn_up.weight",
        blk + "ffn_down.weight",       blk + "ffn_norm.weight",
        blk + "post_ffw_norm.weight",  blk + "pre_ffw_norm_2.weight",
        blk + "ffn_gate_shexp.weight", blk + "ffn_up_shexp.weight",
        blk + "ffn_down_shexp.weight", blk + "per_layer_proj.weight",
        "token_embd.weight",           "output_norm.weight",
    };
}

}  // namespace

SCENARIO("v2.13.0: the expert pattern is llama.cpp's, not our invention",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("the vendored llama.cpp at this pin") {
        THEN("our copy of the expert tail regex is byte-identical") {
            // If this fires, upstream renamed the expert tensors or widened
            // the alternation. Copy the new value — do NOT relax the test:
            // a stale pattern matches nothing and offloads nothing while
            // every other gate stays green.
            CHECK(std::string(kExpertTensorRegex)
                  == std::string(LLM_FFN_EXPS_REGEX));
        }

        THEN("a per-layer pattern is assembled the way upstream assembles it") {
            // llm_ffn_block_regex(idx, LLM_FFN_EXPS_REGEX)
            CHECK(expert_block_pattern(0)
                  == "blk\\.0\\.ffn_(up|down|gate|gate_up)_(ch|)exps");
            CHECK(expert_block_pattern(17)
                  == "blk\\.17\\.ffn_(up|down|gate|gate_up)_(ch|)exps");
        }
    }
}

SCENARIO("v2.13.0: the patterns select gemma4 expert tensors and nothing else",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("the pattern for layer 3") {
        const std::string pattern = expert_block_pattern(3);

        WHEN("it is matched against that layer's routed-expert tensors") {
            THEN("every one of them is selected for the host") {
                for (const auto& name : gemma4_expert_tensors(3)) {
                    INFO("tensor: " << name);
                    CHECK(matches(pattern, name));
                }
            }
        }

        WHEN("it is matched against everything else in the layer") {
            THEN("attention, KV-bearing weights, router, dense FFN and norms "
                 "are untouched") {
                for (const auto& name : gemma4_resident_tensors(3)) {
                    INFO("tensor: " << name);
                    CHECK_FALSE(matches(pattern, name));
                }
            }
        }

        WHEN("it is matched against a DIFFERENT layer's expert tensors") {
            THEN("the layer index is a hard boundary") {
                for (const auto& name : gemma4_expert_tensors(4)) {
                    INFO("tensor: " << name);
                    CHECK_FALSE(matches(pattern, name));
                }
            }
        }
    }

    GIVEN("the pattern for layer 1 and a two-digit layer name") {
        // `blk.1` followed by a literal dot — so `blk.10...` cannot match,
        // which is the prefix bug an unanchored pattern would have.
        THEN("blk.10 is not caught by blk.1") {
            CHECK_FALSE(matches(expert_block_pattern(1),
                                "blk.10.ffn_up_exps.weight"));
            CHECK(matches(expert_block_pattern(10),
                          "blk.10.ffn_up_exps.weight"));
        }
    }
}

SCENARIO("v2.13.0: the override array is a valid, owning C contract",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("four layers of expert offload") {
        const ExpertOffloadOverrides overrides(
            4, ggml_backend_cpu_buffer_type());

        THEN("there are exactly four entries, in layer order") {
            REQUIRE(overrides.size() == 4);
            REQUIRE(overrides.data() != nullptr);
            for (int i = 0; i < 4; ++i) {
                INFO("entry " << i);
                CHECK(std::string(overrides.data()[i].pattern)
                      == expert_block_pattern(i));
                CHECK(overrides.data()[i].buft
                      == ggml_backend_cpu_buffer_type());
            }
        }

        THEN("the array is null-terminated, as llama.cpp's walk requires") {
            // llama-model-loader.cpp iterates `overrides->pattern != nullptr`.
            CHECK(overrides.data()[4].pattern == nullptr);
        }

        THEN("every pattern points into storage the object owns") {
            for (std::size_t i = 0; i < overrides.size(); ++i) {
                INFO("entry " << i);
                CHECK(overrides.data()[i].pattern
                      == overrides.patterns()[i].c_str());
            }
        }

        THEN("the strings outlive the construction call and survive churn") {
            // llama.cpp dereferences these pointers throughout tensor
            // placement, long after the params struct was built. Capture the
            // raw pointers, then allocate hard enough to reuse any freed
            // temporary, and read them back.
            std::vector<const char*> captured;
            for (std::size_t i = 0; i < overrides.size(); ++i) {
                captured.push_back(overrides.data()[i].pattern);
            }

            std::vector<std::string> churn;
            churn.reserve(4096);
            for (int i = 0; i < 4096; ++i) {
                churn.emplace_back(128, static_cast<char>('a' + (i % 26)));
            }

            for (std::size_t i = 0; i < captured.size(); ++i) {
                INFO("entry " << i);
                CHECK(std::string(captured[i])
                      == expert_block_pattern(static_cast<int>(i)));
            }
        }
    }
}

SCENARIO("v2.13.0: an absent cpu_moe_layers installs no overrides at all",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("a ModelConfig nobody set cpu_moe_layers on") {
        const ModelConfig cfg;

        THEN("the key defaults to off") {
            CHECK(cfg.cpu_moe_layers == 0);
        }

        THEN("no patterns are built") {
            CHECK(expert_offload_patterns(cfg.cpu_moe_layers).empty());
        }

        THEN("the override pointer handed to llama.cpp is null — the same "
             "value llama_model_default_params() already carries") {
            const ExpertOffloadOverrides overrides(
                cfg.cpu_moe_layers, ggml_backend_cpu_buffer_type());
            CHECK(overrides.size() == 0);
            CHECK(overrides.data() == nullptr);
        }

        THEN("nothing is refused") {
            CHECK(expert_offload_conflict_reason(cfg).empty());
            CHECK(expert_offload_load_refusal(cfg.cpu_moe_layers, 30, 0)
                      .empty());
        }
    }

    GIVEN("an explicitly zero cpu_moe_layers") {
        ModelConfig cfg;
        cfg.cpu_moe_layers = 0;
        cfg.gpu_layers = 0;
        cfg.gpu_layers_auto = true;

        THEN("off is off — not even the conflicting combinations fire") {
            CHECK(expert_offload_patterns(0).empty());
            CHECK(expert_offload_conflict_reason(cfg).empty());
        }
    }
}

SCENARIO("v2.13.0: impossible expert-offload combinations are refused loudly",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("a negative count") {
        ModelConfig cfg;
        cfg.cpu_moe_layers = -1;
        cfg.gpu_layers = 18;

        THEN("it is refused, not read as an 'all layers' sentinel") {
            const auto why = expert_offload_conflict_reason(cfg);
            REQUIRE_FALSE(why.empty());
            INFO(why);
            CHECK(why.find("cpu_moe_layers") != std::string::npos);
            CHECK(why.find("sentinel") != std::string::npos);
        }
    }

    GIVEN("expert offload on a CPU-only tier") {
        ModelConfig cfg;
        cfg.cpu_moe_layers = 8;
        cfg.gpu_layers = 0;

        THEN("it is refused rather than accepted as a no-op") {
            const auto why = expert_offload_conflict_reason(cfg);
            REQUIRE_FALSE(why.empty());
            INFO(why);
            CHECK(why.find("gpu_layers: 0") != std::string::npos);
        }
    }

    GIVEN("expert offload alongside gpu_layers: auto") {
        ModelConfig cfg;
        cfg.cpu_moe_layers = 8;
        cfg.gpu_layers_auto = true;

        // v2.13.1: still refused, and for a BETTER reason. Auto now derives
        // the expert split itself from the GGUF's real expert-tensor bytes,
        // so this is no longer "auto cannot model expert placement" — it is
        // "you asked two things to decide one placement". The refusal is
        // unchanged; only its explanation was false and is now true.
        THEN("it is refused, because auto derives the split itself") {
            const auto why = expert_offload_conflict_reason(cfg);
            REQUIRE_FALSE(why.empty());
            INFO(why);
            CHECK(why.find("gpu_layers: auto") != std::string::npos);
            CHECK(why.find("derives the expert split") != std::string::npos);
            CHECK(why.find("let auto choose") != std::string::npos);
        }
    }

    GIVEN("a valid explicit split") {
        ModelConfig cfg;
        cfg.cpu_moe_layers = 12;
        cfg.gpu_layers = 31;

        THEN("it is admitted") {
            CHECK(expert_offload_conflict_reason(cfg).empty());
        }
    }
}

SCENARIO("v2.13.0: the load-time refusals need the model, and say so",
         "[expert_offload][gh153][2.13.0]") {
    GIVEN("a dense model — no experts declared") {
        THEN("expert offload is refused instead of placing nothing") {
            const auto why = expert_offload_load_refusal(8, 30, 0);
            REQUIRE_FALSE(why.empty());
            INFO(why);
            CHECK(why.find("no experts") != std::string::npos);
        }
    }

    GIVEN("a count beyond the model's layer count") {
        THEN("it is refused, not clamped") {
            const auto why = expert_offload_load_refusal(40, 31, 128);
            REQUIRE_FALSE(why.empty());
            INFO(why);
            CHECK(why.find("31") != std::string::npos);
            CHECK(why.find("not clamped") != std::string::npos);
        }
    }

    GIVEN("a MoE model with room for the request") {
        THEN("every count up to the layer count is admitted") {
            CHECK(expert_offload_load_refusal(1, 31, 128).empty());
            CHECK(expert_offload_load_refusal(18, 31, 128).empty());
            CHECK(expert_offload_load_refusal(31, 31, 128).empty());
        }
    }

    GIVEN("the feature off on a dense model") {
        THEN("nothing is refused — off is unconditionally acceptable") {
            CHECK(expert_offload_load_refusal(0, 30, 0).empty());
            CHECK(expert_offload_load_refusal(-1, 30, 0).empty());
        }
    }
}
