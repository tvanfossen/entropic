// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_grammar_registry.cpp
 * @brief Tests for GrammarRegistry — named grammar management.
 *
 * Tests cover: register, deregister, get, has, entry, list, clear,
 * validate, load_bundled, duplicate key rejection, and thread safety.
 *
 * @version 1.9.3
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/grammar_registry.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Valid GBNF grammar for testing.
const std::string VALID_GBNF = R"(root ::= "hello")";

/// Another valid GBNF grammar.
const std::string VALID_GBNF_2 = R"(root ::= "world")";

/// Invalid GBNF grammar for testing.
const std::string INVALID_GBNF = R"(root ::= invalid[)";

/**
 * @brief Create a temporary directory with .gbnf files for testing.
 * @param dir_path Path to create.
 * @param files Map of filename -> content.
 * @utility
 * @version 1.9.3
 */
void create_grammar_dir(
    const std::filesystem::path& dir_path,
    const std::vector<std::pair<std::string, std::string>>& files)
{
    std::filesystem::create_directories(dir_path);
    for (const auto& [name, content] : files) {
        std::ofstream f(dir_path / name);
        f << content;
    }
}

/**
 * @brief Clean up a temporary directory.
 * @param dir_path Path to remove.
 * @utility
 * @version 1.9.3
 */
void cleanup_dir(const std::filesystem::path& dir_path) {
    std::filesystem::remove_all(dir_path);
}

} // anonymous namespace

SCENARIO("Register grammar from in-memory string",
         "[grammar][registry][register]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;

        WHEN("a valid grammar is registered") {
            bool ok = reg.register_grammar("test", VALID_GBNF, "runtime");

            THEN("registration succeeds") {
                REQUIRE(ok);
                REQUIRE(reg.has("test"));
                REQUIRE(reg.get("test") == VALID_GBNF);
                REQUIRE(reg.size() == 1);
            }

            THEN("entry metadata is correct") {
                auto e = reg.entry("test");
                REQUIRE(e.key == "test");
                REQUIRE(e.source == "runtime");
                REQUIRE(e.validated == true);
                REQUIRE(e.error.empty());
            }
        }
    }
}

SCENARIO("Duplicate key registration fails",
         "[grammar][registry][register]")
{
    GIVEN("a registry with 'test' registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("test", VALID_GBNF);

        WHEN("registering another grammar with key 'test'") {
            bool ok = reg.register_grammar("test", VALID_GBNF_2);

            THEN("registration fails") {
                REQUIRE_FALSE(ok);
            }

            THEN("original content is preserved") {
                REQUIRE(reg.get("test") == VALID_GBNF);
            }
        }
    }
}

SCENARIO("Invalid grammar registered with error metadata",
         "[grammar][registry][validate]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;

        WHEN("an invalid grammar is registered") {
            bool ok = reg.register_grammar("bad", INVALID_GBNF);

            THEN("registration succeeds (grammar is stored)") {
                REQUIRE(ok);
                REQUIRE(reg.has("bad"));
                REQUIRE(reg.get("bad") == INVALID_GBNF);
            }

            THEN("entry has validation error metadata") {
                auto e = reg.entry("bad");
                REQUIRE_FALSE(e.validated);
                REQUIRE_FALSE(e.error.empty());
            }
        }
    }
}

SCENARIO("Deregister removes grammar",
         "[grammar][registry][deregister]")
{
    GIVEN("a registry with 'test' registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("test", VALID_GBNF);

        WHEN("deregister is called") {
            bool ok = reg.deregister("test");

            THEN("removal succeeds") {
                REQUIRE(ok);
                REQUIRE_FALSE(reg.has("test"));
                REQUIRE(reg.get("test").empty());
                REQUIRE(reg.size() == 0);
            }
        }
    }
}

SCENARIO("Deregister nonexistent key returns false",
         "[grammar][registry][deregister]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;

        WHEN("deregistering a nonexistent key") {
            bool ok = reg.deregister("nonexistent");

            THEN("it returns false") {
                REQUIRE_FALSE(ok);
            }
        }
    }
}

SCENARIO("Get nonexistent key returns empty string",
         "[grammar][registry][get]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;

        THEN("get returns empty string") {
            REQUIRE(reg.get("nonexistent").empty());
        }

        THEN("entry returns entry with empty key") {
            auto e = reg.entry("nonexistent");
            REQUIRE(e.key.empty());
        }
    }
}

SCENARIO("Validate accepts well-formed GBNF",
         "[grammar][validate]")
{
    THEN("valid grammar returns empty string") {
        REQUIRE(entropic::GrammarRegistry::validate(VALID_GBNF).empty());
    }
}

SCENARIO("Validate rejects malformed GBNF",
         "[grammar][validate]")
{
    THEN("invalid grammar returns non-empty error") {
        auto err = entropic::GrammarRegistry::validate(INVALID_GBNF);
        REQUIRE_FALSE(err.empty());
    }
}

SCENARIO("Validate rejects empty string",
         "[grammar][validate]")
{
    THEN("empty grammar returns error") {
        auto err = entropic::GrammarRegistry::validate("");
        REQUIRE_FALSE(err.empty());
    }
}

SCENARIO("List returns all registered grammars",
         "[grammar][registry][list]")
{
    GIVEN("3 grammars registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("a", VALID_GBNF, "runtime");
        reg.register_grammar("b", VALID_GBNF_2, "dynamic");
        reg.register_grammar("c", INVALID_GBNF, "file");

        WHEN("list is called") {
            auto entries = reg.list();

            THEN("it returns 3 entries") {
                REQUIRE(entries.size() == 3);
            }

            THEN("content is not included in list entries") {
                for (const auto& e : entries) {
                    REQUIRE(e.gbnf_content.empty());
                }
            }
        }
    }
}

SCENARIO("Clear removes all grammars",
         "[grammar][registry][clear]")
{
    GIVEN("3 grammars registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("a", VALID_GBNF);
        reg.register_grammar("b", VALID_GBNF_2);
        reg.register_grammar("c", VALID_GBNF);

        WHEN("clear is called") {
            reg.clear();

            THEN("registry is empty") {
                REQUIRE(reg.size() == 0);
                REQUIRE_FALSE(reg.has("a"));
            }
        }
    }
}

SCENARIO("Load bundled grammars from directory",
         "[grammar][registry][bundled]")
{
    auto test_dir = std::filesystem::temp_directory_path()
                    / "entropic_grammar_test";

    GIVEN("a directory with 3 .gbnf files") {
        create_grammar_dir(test_dir, {
            {"alpha.gbnf", VALID_GBNF},
            {"beta.gbnf", VALID_GBNF_2},
            {"gamma.gbnf", INVALID_GBNF},
        });

        entropic::GrammarRegistry reg;

        WHEN("load_bundled is called") {
            size_t count = reg.load_bundled(test_dir);

            THEN("all 3 are loaded") {
                REQUIRE(count == 3);
                REQUIRE(reg.has("alpha"));
                REQUIRE(reg.has("beta"));
                REQUIRE(reg.has("gamma"));
            }

            THEN("valid files have validated=true") {
                REQUIRE(reg.entry("alpha").validated);
                REQUIRE(reg.entry("beta").validated);
            }

            THEN("invalid file has validated=false") {
                REQUIRE_FALSE(reg.entry("gamma").validated);
                REQUIRE_FALSE(reg.entry("gamma").error.empty());
            }

            THEN("all entries have source 'bundled'") {
                REQUIRE(reg.entry("alpha").source == "bundled");
                REQUIRE(reg.entry("beta").source == "bundled");
            }
        }

        cleanup_dir(test_dir);
    }
}

SCENARIO("Load bundled from nonexistent directory",
         "[grammar][registry][bundled]")
{
    GIVEN("a nonexistent directory") {
        entropic::GrammarRegistry reg;

        WHEN("load_bundled is called") {
            size_t count = reg.load_bundled("/nonexistent/path");

            THEN("it returns 0") {
                REQUIRE(count == 0);
            }
        }
    }
}

SCENARIO("Register grammar from file",
         "[grammar][registry][file]")
{
    auto test_dir = std::filesystem::temp_directory_path()
                    / "entropic_grammar_file_test";
    create_grammar_dir(test_dir, {{"custom.gbnf", VALID_GBNF}});

    GIVEN("a .gbnf file on disk") {
        entropic::GrammarRegistry reg;

        WHEN("register_from_file is called with explicit key") {
            bool ok = reg.register_from_file("mykey", test_dir / "custom.gbnf");

            THEN("registration succeeds") {
                REQUIRE(ok);
                REQUIRE(reg.has("mykey"));
                REQUIRE(reg.get("mykey") == VALID_GBNF);
                REQUIRE(reg.entry("mykey").source == "file");
            }
        }

        WHEN("register_from_file is called with empty key") {
            bool ok = reg.register_from_file("", test_dir / "custom.gbnf");

            THEN("key is derived from filename stem") {
                REQUIRE(ok);
                REQUIRE(reg.has("custom"));
            }
        }
    }

    cleanup_dir(test_dir);
}

SCENARIO("Register from nonexistent file fails",
         "[grammar][registry][file]")
{
    GIVEN("a nonexistent file") {
        entropic::GrammarRegistry reg;

        WHEN("register_from_file is called") {
            bool ok = reg.register_from_file("bad", "/no/such/file.gbnf");

            THEN("registration fails") {
                REQUIRE_FALSE(ok);
                REQUIRE_FALSE(reg.has("bad"));
            }
        }
    }
}

SCENARIO("Concurrent register and get are thread-safe",
         "[grammar][registry][thread]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;
        constexpr int N = 100;

        WHEN("thread A registers while thread B reads") {
            std::thread writer([&] {
                for (int i = 0; i < N; ++i) {
                    reg.register_grammar(
                        "key_" + std::to_string(i), VALID_GBNF);
                }
            });

            std::thread reader([&] {
                for (int i = 0; i < N; ++i) {
                    reg.get("key_" + std::to_string(i));
                    reg.has("key_" + std::to_string(i));
                }
            });

            writer.join();
            reader.join();

            THEN("all registrations completed") {
                REQUIRE(reg.size() == N);
            }
        }
    }
}

SCENARIO("GrammarEntry default-constructed is empty",
         "[grammar][types]")
{
    GIVEN("a default GrammarEntry") {
        entropic::GrammarEntry entry;

        THEN("all fields are empty/false") {
            REQUIRE(entry.key.empty());
            REQUIRE(entry.gbnf_content.empty());
            REQUIRE(entry.source.empty());
            REQUIRE(entry.validated == false);
            REQUIRE(entry.error.empty());
        }
    }
}

SCENARIO("GenerationParams grammar_key defaults empty",
         "[grammar][params]")
{
    GIVEN("a default GenerationParams") {
        entropic::GenerationParams params;

        THEN("grammar_key is empty") {
            REQUIRE(params.grammar_key.empty());
        }
    }
}
