/**
 * @file test_config_structs.cpp
 * @brief BDD tests for config struct default values.
 * @version 1.8.0
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/types/config.h>

SCENARIO("ModelConfig has correct defaults", "[config][types]") {
    GIVEN("A default-constructed ModelConfig") {
        entropic::ModelConfig config;

        THEN("adapter defaults to qwen35") {
            REQUIRE(config.adapter == "qwen35");
        }

        THEN("context_length defaults to 16384") {
            REQUIRE(config.context_length == 16384);
        }

        THEN("gpu_layers defaults to -1 (all)") {
            REQUIRE(config.gpu_layers == -1);
        }

        THEN("keep_warm defaults to false") {
            REQUIRE(config.keep_warm == false);
        }

        THEN("use_mlock defaults to true") {
            REQUIRE(config.use_mlock == true);
        }

        THEN("flash_attn defaults to true") {
            REQUIRE(config.flash_attn == true);
        }

        THEN("n_batch defaults to 512") {
            REQUIRE(config.n_batch == 512);
        }

        THEN("n_threads defaults to 0 (auto)") {
            REQUIRE(config.n_threads == 0);
        }

        THEN("cache_type_k defaults to f16") {
            REQUIRE(config.cache_type_k == "f16");
        }

        THEN("cache_type_v defaults to f16") {
            REQUIRE(config.cache_type_v == "f16");
        }

        THEN("reasoning_budget defaults to -1 (unlimited)") {
            REQUIRE(config.reasoning_budget == -1);
        }

        THEN("allowed_tools defaults to nullopt (all tools)") {
            REQUIRE(!config.allowed_tools.has_value());
        }
    }
}

SCENARIO("GenerationParams has correct defaults", "[config][types]") {
    GIVEN("A default-constructed GenerationParams") {
        entropic::GenerationParams params;

        THEN("temperature defaults to 0.7") {
            REQUIRE(params.temperature == Catch::Approx(0.7f));
        }

        THEN("top_p defaults to 0.9") {
            REQUIRE(params.top_p == Catch::Approx(0.9f));
        }

        THEN("top_k defaults to 40") {
            REQUIRE(params.top_k == 40);
        }

        THEN("max_tokens defaults to 4096") {
            REQUIRE(params.max_tokens == 4096);
        }

        THEN("repeat_penalty defaults to 1.1") {
            REQUIRE(params.repeat_penalty == Catch::Approx(1.1f));
        }

        THEN("reasoning_budget defaults to -1 (model default)") {
            REQUIRE(params.reasoning_budget == -1);
        }

        THEN("grammar defaults to empty (unconstrained)") {
            REQUIRE(params.grammar.empty());
        }
    }
}
