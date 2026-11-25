// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_required_grammar_test.cpp
 * @brief CPU unit tests for the Gemma 4 REQUIRED root-rule patch (v2.14.6).
 *
 * Synthetic GBNF in the pinned upstream shape; no model file. The real-template
 * behaviour (what the patched grammar accepts and rejects) is covered by
 * tests/model/test_v2146_gemma4_required_grammar.cpp.
 *
 * @version 2.14.6
 */

#include <catch2/catch_test_macros.hpp>

#include "gemma4_required_grammar.h"

#include <string>

namespace {

/// Root and neighbours as common_chat_params_init_gemma4 renders them at the pin.
std::string pinned_gbnf(const std::string& root_line) {
    return "content ::= until-164\n"
           "message ::= (thought | ) content\n"
           + root_line + "\n"
           "scan-to-toolcall ::= until-161\n"
           "thought ::= \"<|channel>thought\" space until-5 \"<channel|>\"\n";
}

} // namespace

SCENARIO("The Gemma 4 REQUIRED root is rewritten to the f072b10371 shape",
         "[2.14.6][grammar][gemma4][inference][cpu]")
{
    GIVEN("a GBNF whose root is the pinned pre-fix shape") {
        const auto in = pinned_gbnf(
            "root ::= start message* scan-to-toolcall tool-call");
        WHEN("the patch runs") {
            const auto out = entropic::patch_gemma4_required_root(in);
            THEN("the root becomes start, optional thought, tool call") {
                CHECK(entropic::gbnf_root_line(out)
                      == "root ::= start (thought | ) tool-call");
            }
            AND_THEN("every other rule is untouched") {
                CHECK(out == pinned_gbnf("root ::= start (thought | ) tool-call"));
            }
        }
    }
    GIVEN("the root line is the first line of the grammar") {
        const std::string in =
            "root ::= start message* scan-to-toolcall tool-call\nx ::= \"a\"\n";
        THEN("it is still rewritten") {
            CHECK(entropic::patch_gemma4_required_root(in)
                  == "root ::= start (thought | ) tool-call\nx ::= \"a\"\n");
        }
    }
}

SCENARIO("An unknown Gemma 4 root shape is left unchanged",
         "[2.14.6][grammar][gemma4][inference][cpu]")
{
    WHEN("the root is already the fixed shape (a newer pin)") {
        const auto in = pinned_gbnf("root ::= start (thought | ) tool-call");
        THEN("the grammar is returned unchanged") {
            CHECK(entropic::patch_gemma4_required_root(in) == in);
        }
    }
    WHEN("the root carries extra rules after the expected prefix") {
        const auto in = pinned_gbnf(
            "root ::= start message* scan-to-toolcall tool-call extra");
        THEN("it does not match by prefix and is unchanged") {
            CHECK(entropic::patch_gemma4_required_root(in) == in);
        }
    }
    WHEN("there is no root rule at all") {
        const std::string in = "x ::= \"a\"\nyroot ::= x\n";
        THEN("the grammar is unchanged and the root line is empty") {
            CHECK(entropic::patch_gemma4_required_root(in) == in);
            CHECK(entropic::gbnf_root_line(in).empty());
        }
    }
}
