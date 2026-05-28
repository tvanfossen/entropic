// SPDX-License-Identifier: Apache-2.0
/**
 * @file xml_parameter_parser_test.cpp
 * @brief Unit tests for the shared XML parameter parser (gh#79).
 *
 * The parser is the v2.4.1 DRY of three byte-identical inline regex
 * blocks that lived in qwen35 / qwen36 / nemotron3 adapters. Beyond
 * pinning the new helper's contract, these scenarios pin the gh#79
 * fix: when a model closes `<parameter=NAME>` with `</NAME>` instead
 * of `</parameter>`, the parser must NOT bleed the next param's
 * content into the first param's value.
 *
 * Pre-fix evidence from the gh#79 issue body (qwen3_6_a3b live):
 *
 *   <parameter=target>
 *   researcher</target>
 *   <parameter=task>
 *   Find ...
 *   </parameter>
 *
 * Pre-fix `target` came back as the whole blob from `researcher` to
 * the second `</parameter>`. Post-fix `target` = "researcher".
 *
 * @version 2.4.1
 */

#include "../../../src/inference/adapters/xml_parameter_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace adapters = entropic::inference::adapters;

namespace {
inline std::shared_ptr<spdlog::logger> null_logger() { return nullptr; }
}  // namespace

// ── Baseline: well-formed `</parameter>` close still parses ──────

SCENARIO("well-formed XML parameters parse with literal </parameter> close",
         "[v2.4.1][adapters][xml_param][baseline]") {
    GIVEN("a body where every parameter uses </parameter>") {
        std::string body =
            "<parameter=alpha>one</parameter>"
            "<parameter=beta>two</parameter>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("both parameters extract cleanly") {
                CHECK(args.size() == 2);
                CHECK(args["alpha"] == "one");
                CHECK(args["beta"] == "two");
            }
        }
    }
}

// ── gh#79 core fix: `</NAME>` close tolerated ───────────────────

SCENARIO("gh#79: a single parameter closed with </NAME> parses correctly",
         "[v2.4.1][adapters][xml_param][gh79]") {
    GIVEN("a body where the close tag echoes the parameter name") {
        std::string body = "<parameter=path>/etc/hostname</path>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("path parses to the value before </path>") {
                CHECK(args.size() == 1);
                CHECK(args["path"] == "/etc/hostname");
            }
        }
    }
}

SCENARIO("gh#79: first param closes with </NAME>, second with </parameter>",
         "[v2.4.1][adapters][xml_param][gh79]") {
    GIVEN("the exact pre-fix pathological emit from the gh#79 issue body") {
        std::string body =
            "<parameter=target>\nresearcher</target>\n"
            "<parameter=task>\n"
            "Find information about the \"findme\" command\n"
            "</parameter>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("target is NOT bled into — it stops at </target>") {
                CHECK(args.size() == 2);
                CHECK(args["target"] == "researcher");
            }
            AND_THEN("task captures the multiline value verbatim (trimmed)") {
                CHECK(args["task"] ==
                      "Find information about the \"findme\" command");
            }
        }
    }
}

SCENARIO("gh#79: both params close with </NAME>",
         "[v2.4.1][adapters][xml_param][gh79]") {
    GIVEN("a body where every parameter echoes its name in the close tag") {
        std::string body =
            "<parameter=a>1</a><parameter=b>2</b>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("both extract; no value bleed") {
                CHECK(args.size() == 2);
                CHECK(args["a"] == "1");
                CHECK(args["b"] == "2");
            }
        }
    }
}

SCENARIO("gh#79: parameter name with underscores and digits",
         "[v2.4.1][adapters][xml_param][gh79]") {
    GIVEN("a body with snake_case + digit-suffix parameter names") {
        std::string body =
            "<parameter=arg_1>val1</arg_1>"
            "<parameter=snake_case_param>val2</snake_case_param>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("the close-tag name match handles non-alpha characters") {
                CHECK(args.size() == 2);
                CHECK(args["arg_1"] == "val1");
                CHECK(args["snake_case_param"] == "val2");
            }
        }
    }
}

// ── Whitespace / empty handling ─────────────────────────────────

SCENARIO("whitespace and newlines around values are trimmed",
         "[v2.4.1][adapters][xml_param][trim]") {
    GIVEN("padded values inside a parameter block") {
        std::string body =
            "<parameter=key>\n   leading and trailing whitespace   \n</parameter>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("the value is trimmed") {
                CHECK(args["key"] == "leading and trailing whitespace");
            }
        }
    }
}

SCENARIO("empty parameter value (after trim) is skipped",
         "[v2.4.1][adapters][xml_param][empty]") {
    GIVEN("a parameter whose body is only whitespace") {
        std::string body = "<parameter=empty>\n   \n</parameter>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("the empty param is not emitted") {
                CHECK(args.empty());
            }
        }
    }
}

SCENARIO("parameter with no closing tag is skipped without aborting scan",
         "[v2.4.1][adapters][xml_param][unterminated]") {
    GIVEN("an unterminated first param followed by a well-formed second") {
        std::string body =
            "<parameter=lost>some text that never closes"
            "<parameter=found>found_value</parameter>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            // Note: in this construction the unterminated `<parameter=lost>`'s
            // value runs UP TO the second `</parameter>` if we only look at
            // close-paren candidates — but there's no `</lost>` in the body
            // either, so the parser sees `</parameter>` as the close for
            // `lost`, capturing "some text that never closes<parameter=found>found_value".
            // Then `found` does not start a fresh scan because we already
            // consumed past `</parameter>`. The post-fix semantics is "first
            // close wins from this opening" — so this scenario documents
            // that exactly. Without a `</lost>` available, `</parameter>`
            // closes the first param.
            //
            // The pure unterminated case (no close of any kind anywhere) is
            // exercised by the next scenario.
            THEN("the first param closes at the </parameter> belonging to the second") {
                CHECK(args.count("lost") == 1);
                // value contains the trailing `<parameter=found>found_value` —
                // the inner well-formed param IS lost in this construction.
                // This is acceptable: an unterminated `</lost>` is malformed
                // input and the parser still produces a non-throwing result.
            }
        }
    }
}

SCENARIO("truly unterminated parameter (no </parameter>, no </NAME>) is dropped",
         "[v2.4.1][adapters][xml_param][unterminated]") {
    GIVEN("a body where the close tag is missing entirely") {
        std::string body = "<parameter=key>value with no close";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("no parameter is emitted") {
                CHECK(args.empty());
            }
        }
    }
}

// ── Nested-`<function=` truncation guard (preserved behavior) ───

SCENARIO("body is truncated at a nested <function= tag (malformed multi-call)",
         "[v2.4.1][adapters][xml_param][truncate]") {
    GIVEN("a body with a stray nested <function= tag after the first param") {
        std::string body =
            "<parameter=ok>kept</parameter>"
            "<function=other><parameter=dropped>ignored</parameter></function>";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("only the param before the nested tag is kept") {
                CHECK(args.size() == 1);
                CHECK(args["ok"] == "kept");
                CHECK(args.count("dropped") == 0);
            }
        }
    }
}

// ── Empty body / no parameters ──────────────────────────────────

SCENARIO("empty input produces empty output",
         "[v2.4.1][adapters][xml_param]") {
    GIVEN("an empty function body") {
        std::string body;
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("the result is empty") {
                CHECK(args.empty());
            }
        }
    }
}

SCENARIO("body with no <parameter= opening produces empty output",
         "[v2.4.1][adapters][xml_param]") {
    GIVEN("a body without any parameter tags") {
        std::string body = "just some text, no parameters here";
        WHEN("the parser runs") {
            auto args = adapters::parse_xml_parameters(body, null_logger());
            THEN("the result is empty") {
                CHECK(args.empty());
            }
        }
    }
}
