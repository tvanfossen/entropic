// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file yaml_util_test.cpp
 * @brief BDD tests for ryml extraction helpers.
 * @version 1.10.0
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <ryml.hpp>
#include <c4/std/string.hpp>

// Internal header — test accesses config internals
#include "yaml_util.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace entropic::config;
using Catch::Approx;

/**
 * @brief Parse a YAML string into a ryml tree and return root node.
 * @param yaml YAML content.
 * @return Root node reference.
 * @version 1.10.0
 * @internal
 */
static ryml::Tree parse_yaml(const std::string& yaml)
{
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::csubstr(yaml.data(), yaml.size()));
    return tree;
}

// ── extract string ───────────────────────────────────────

SCENARIO("extract string from YAML node", "[config][yaml_util]") {
    GIVEN("a map with a string value") {
        auto tree = parse_yaml("name: hello");
        auto root = tree.rootref();
        std::string out;

        WHEN("extracting an existing key") {
            bool found = extract(root, "name", out);
            THEN("it returns true and sets the value") {
                REQUIRE(found);
                REQUIRE(out == "hello");
            }
        }

        WHEN("extracting a missing key") {
            out = "unchanged";
            bool found = extract(root, "missing", out);
            THEN("it returns false and leaves output unchanged") {
                REQUIRE_FALSE(found);
                REQUIRE(out == "unchanged");
            }
        }
    }

    GIVEN("a map with a null value") {
        auto tree = parse_yaml("name: ~");
        auto root = tree.rootref();
        std::string out = "unchanged";

        WHEN("extracting the null key") {
            bool found = extract(root, "name", out);
            THEN("it returns false") {
                REQUIRE_FALSE(found);
                REQUIRE(out == "unchanged");
            }
        }
    }
}

// ── extract int ──────────────────────────────────────────

SCENARIO("extract int from YAML node", "[config][yaml_util]") {
    GIVEN("a map with an integer value") {
        auto tree = parse_yaml("count: 42");
        auto root = tree.rootref();
        int out = 0;

        WHEN("extracting the key") {
            bool found = extract(root, "count", out);
            THEN("it returns true with the correct int") {
                REQUIRE(found);
                REQUIRE(out == 42);
            }
        }
    }
}

// ── extract bool ─────────────────────────────────────────

SCENARIO("extract bool from YAML node", "[config][yaml_util]") {
    GIVEN("various truthy YAML values") {
        for (auto& yaml : {"flag: true", "flag: True", "flag: TRUE",
                           "flag: yes", "flag: Yes", "flag: YES",
                           "flag: on", "flag: On", "flag: ON",
                           "flag: 1"}) {
            auto tree = parse_yaml(yaml);
            auto root = tree.rootref();
            bool out = false;

            WHEN("extracting") {
                bool found = extract(root, "flag", out);
                THEN("it returns true") {
                    REQUIRE(found);
                    REQUIRE(out == true);
                }
            }
        }
    }

    GIVEN("a falsy YAML value") {
        auto tree = parse_yaml("flag: false");
        auto root = tree.rootref();
        bool out = true;

        WHEN("extracting") {
            bool found = extract(root, "flag", out);
            THEN("it returns false") {
                REQUIRE(found);
                REQUIRE(out == false);
            }
        }
    }
}

// ── extract_path with ~ expansion ────────────────────────

SCENARIO("extract_path expands home directory", "[config][yaml_util]") {
    GIVEN("a path starting with ~") {
        auto tree = parse_yaml("dir: ~/models");
        auto root = tree.rootref();
        std::filesystem::path out;

        WHEN("extracting with HOME set") {
            bool found = extract_path(root, "dir", out);
            THEN("~ is expanded to HOME") {
                REQUIRE(found);
                REQUIRE(out.string().find('~') == std::string::npos);
                REQUIRE(out.string().find("models") != std::string::npos);
            }
        }
    }

    GIVEN("an absolute path") {
        auto tree = parse_yaml("dir: /opt/data");
        auto root = tree.rootref();
        std::filesystem::path out;

        WHEN("extracting") {
            bool found = extract_path(root, "dir", out);
            THEN("path is unchanged") {
                REQUIRE(found);
                REQUIRE(out == "/opt/data");
            }
        }
    }
}

// ── extract_string_list ──────────────────────────────────

SCENARIO("extract_string_list from YAML sequence", "[config][yaml_util]") {
    GIVEN("a YAML sequence") {
        auto tree = parse_yaml("items:\n  - alpha\n  - beta\n  - gamma");
        auto root = tree.rootref();
        std::vector<std::string> out;

        WHEN("extracting the list") {
            bool found = extract_string_list(root, "items", out);
            THEN("all items are captured in order") {
                REQUIRE(found);
                REQUIRE(out.size() == 3);
                REQUIRE(out[0] == "alpha");
                REQUIRE(out[1] == "beta");
                REQUIRE(out[2] == "gamma");
            }
        }
    }

    GIVEN("a missing key") {
        auto tree = parse_yaml("other: val");
        auto root = tree.rootref();
        std::vector<std::string> out = {"preserved"};

        WHEN("extracting") {
            bool found = extract_string_list(root, "items", out);
            THEN("output is unchanged") {
                REQUIRE_FALSE(found);
                REQUIRE(out.size() == 1);
                REQUIRE(out[0] == "preserved");
            }
        }
    }
}

// ── extract_string_map ───────────────────────────────────

SCENARIO("extract_string_map from YAML mapping", "[config][yaml_util]") {
    GIVEN("a YAML mapping") {
        auto tree = parse_yaml("env:\n  FOO: bar\n  BAZ: qux");
        auto root = tree.rootref();
        std::unordered_map<std::string, std::string> out;

        WHEN("extracting the map") {
            bool found = extract_string_map(root, "env", out);
            THEN("all entries are captured") {
                REQUIRE(found);
                REQUIRE(out.size() == 2);
                REQUIRE(out["FOO"] == "bar");
                REQUIRE(out["BAZ"] == "qux");
            }
        }
    }
}

// ── expand_home standalone ───────────────────────────────

SCENARIO("expand_home handles edge cases", "[config][yaml_util]") {
    GIVEN("a bare tilde") {
        auto result = expand_home(std::filesystem::path("~"));
        THEN("it resolves to HOME") {
            REQUIRE_FALSE(result.string().empty());
            REQUIRE(result.string().find('~') == std::string::npos);
        }
    }

    GIVEN("a path without tilde") {
        auto result = expand_home(std::filesystem::path("/usr/local"));
        THEN("it passes through unchanged") {
            REQUIRE(result == "/usr/local");
        }
    }

    GIVEN("an empty path") {
        auto result = expand_home(std::filesystem::path(""));
        THEN("it passes through unchanged") {
            REQUIRE(result.string().empty());
        }
    }
}
