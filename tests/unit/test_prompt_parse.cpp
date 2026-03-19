/**
 * @file test_prompt_parse.cpp
 * @brief BDD tests for prompt file parsing and identity loading.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/prompts/manager.h>
#include <filesystem>

static std::filesystem::path test_data()
{
    return std::filesystem::path(TEST_DATA_DIR);
}

SCENARIO("Parse constitution prompt", "[prompts][parse]") {
    GIVEN("A constitution.md file") {
        entropic::prompts::ParsedPrompt result;

        WHEN("parse_prompt_file is called") {
            auto err = entropic::prompts::parse_prompt_file(
                test_data() / "prompts" / "test_constitution.md",
                entropic::prompts::PromptType::CONSTITUTION,
                result);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("type and version are correct") {
                REQUIRE(result.type
                        == entropic::prompts::PromptType::CONSTITUTION);
                REQUIRE(result.version == 1);
            }

            THEN("body contains expected content") {
                REQUIRE(result.body.find("Core Principles")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Parse app_context prompt", "[prompts][parse]") {
    GIVEN("An app_context.md file") {
        entropic::prompts::ParsedPrompt result;

        WHEN("parse_prompt_file is called") {
            auto err = entropic::prompts::parse_prompt_file(
                test_data() / "prompts" / "test_app_context.md",
                entropic::prompts::PromptType::APP_CONTEXT,
                result);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("body contains Entropic") {
                REQUIRE(result.body.find("Entropic")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Type mismatch fails", "[prompts][parse]") {
    GIVEN("An identity file loaded as constitution") {
        entropic::prompts::ParsedPrompt result;

        WHEN("parse_prompt_file is called with wrong type") {
            auto err = entropic::prompts::parse_prompt_file(
                test_data() / "prompts" / "test_identity.md",
                entropic::prompts::PromptType::CONSTITUTION,
                result);

            THEN("it fails with type mismatch error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("identity") != std::string::npos);
                REQUIRE(err.find("constitution") != std::string::npos);
            }
        }
    }
}

SCENARIO("Load identity file", "[prompts][identity]") {
    GIVEN("A test identity file") {
        entropic::prompts::ParsedIdentity identity;

        WHEN("load_identity is called") {
            auto err = entropic::prompts::load_identity(
                test_data() / "prompts" / "test_identity.md",
                identity);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("name is correct") {
                REQUIRE(identity.frontmatter.name == "testeng");
            }

            THEN("focus has 2 entries") {
                REQUIRE(identity.frontmatter.focus.size() == 2);
                REQUIRE(identity.frontmatter.focus[0] == "write code");
            }

            THEN("examples has 2 entries") {
                REQUIRE(identity.frontmatter.examples.size() == 2);
            }

            THEN("auto_chain is set") {
                REQUIRE(identity.frontmatter.auto_chain.has_value());
                REQUIRE(*identity.frontmatter.auto_chain == "lead");
            }

            THEN("allowed_tools has 2 entries") {
                REQUIRE(identity.frontmatter.allowed_tools.has_value());
                REQUIRE(identity.frontmatter.allowed_tools->size() == 2);
            }

            THEN("inference params are correct") {
                REQUIRE(identity.frontmatter.temperature
                        == Catch::Approx(0.15f));
                REQUIRE(identity.frontmatter.max_output_tokens == 8192);
                REQUIRE(identity.frontmatter.routable == false);
                REQUIRE(identity.frontmatter.enable_thinking == false);
            }

            THEN("phases are parsed") {
                REQUIRE(identity.frontmatter.phases.has_value());
                REQUIRE(identity.frontmatter.phases->size() == 2);
                REQUIRE(identity.frontmatter.phases->count("default") == 1);
                REQUIRE(identity.frontmatter.phases->count("thinking") == 1);
                auto& think = (*identity.frontmatter.phases)["thinking"];
                REQUIRE(think.temperature == Catch::Approx(0.6f));
                REQUIRE(think.enable_thinking == true);
            }

            THEN("benchmark is parsed") {
                REQUIRE(identity.frontmatter.benchmark.has_value());
                REQUIRE(identity.frontmatter.benchmark->prompts.size() == 1);
                REQUIRE(identity.frontmatter.benchmark->prompts[0]
                            .prompt.find("REST API")
                        != std::string::npos);
            }

            THEN("body contains system prompt") {
                REQUIRE(identity.body.find("software engineer")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Constitution loading with tri-state", "[prompts][constitution]") {
    auto data_dir = test_data();

    GIVEN("Bundled default (path=nullopt, disabled=false)") {
        std::string body;
        auto err = entropic::prompts::load_constitution(
            std::nullopt, false, data_dir, body);

        THEN("it loads bundled constitution") {
            REQUIRE(err.empty());
            REQUIRE(body.find("Core Principles") != std::string::npos);
        }
    }

    GIVEN("Disabled (path=nullopt, disabled=true)") {
        std::string body;
        auto err = entropic::prompts::load_constitution(
            std::nullopt, true, data_dir, body);

        THEN("body is empty") {
            REQUIRE(err.empty());
            REQUIRE(body.empty());
        }
    }
}
