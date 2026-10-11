// SPDX-License-Identifier: Apache-2.0
/**
 * @file filesystem_contract_test.cpp
 * @brief gh#203 (v2.14.0): every filesystem tool emits ONLY what its
 *        outputSchema declares — and every error code it declares is one a
 *        handler can actually produce.
 *
 * Each tool runs a fixture set covering success, every declared error and
 * every declared truncation. Each output is checked with the strict
 * validator (closed objects, enumerated codes). Then the set of codes seen
 * must equal the set declared: a schema may not promise a code nothing
 * emits, and a handler may not emit one nothing declares.
 *
 * Also the behaviour the old shapes hid: numbered lines (gh#201), paging
 * and single-file grep (gh#202), edit_file's AFTER semantics and the
 * `insert_line: 0` no-op (gh#203).
 *
 * @version 2.14.0
 */

#include <entropic/mcp/servers/filesystem.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/types/config.h>

#include "../output_contract.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using entropic::FilesystemConfig;
using entropic::FilesystemServer;

namespace {

/// @brief A scratch project root, removed on scope exit.
struct Root {
    fs::path dir;
    Root() {
        static int n = 0;
        dir = fs::temp_directory_path() / ("entropic_fs_contract_"
            + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Root() { std::error_code ec; fs::remove_all(dir, ec); }
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
    void write(const std::string& rel, const std::string& content) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << content;
    }
    std::string read(const std::string& rel) const {
        std::ifstream in(dir / rel, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

/// @brief The tool's declared output schema, from the bundled descriptor.
json output_schema(const std::string& tool) {
    auto def = entropic::load_tool_definition(
        tool, "filesystem", std::string(TEST_DATA_DIR) + "/tools");
    REQUIRE_FALSE(def.output_schema.empty());
    return json::parse(def.output_schema);
}

/// @brief Every `error` enum value the schema declares.
std::set<std::string> declared_codes(const json& schema) {
    std::set<std::string> codes;
    for (const auto& branch : schema.value("oneOf", json::array())) {
        if (branch.value("title", "") == "error") {
            for (const auto& c : branch["properties"]["error"]["enum"]) {
                codes.insert(c.get<std::string>());
            }
        }
    }
    return codes;
}

/// @brief Runs fixtures for one tool, checking each output strictly.
class ContractRun {
public:
    ContractRun(FilesystemServer& server, std::string tool)
        : server_(server), tool_(std::move(tool)),
          schema_(output_schema(tool_)) {}

    /// @brief Call the tool; assert the output conforms; return it.
    json call(const json& args) {
        auto env = json::parse(server_.execute(tool_, args.dump()));
        auto text = env.at("result").get<std::string>();
        INFO(tool_ << " " << args.dump() << " -> " << text);
        auto out = json::parse(text, nullptr, false);
        REQUIRE_FALSE(out.is_discarded());
        CHECK(entropic::test::contract_violation(schema_, out).empty());
        INFO(entropic::test::contract_violation(schema_, out));
        if (out.contains("error")) { seen_.insert(out["error"].get<std::string>()); }
        return out;
    }

    /// @brief Every declared code was produced, and nothing else was.
    void require_codes_exercised() const {
        INFO(tool_);
        CHECK(seen_ == declared_codes(schema_));
    }

private:
    FilesystemServer& server_;
    std::string tool_;
    json schema_;
    std::set<std::string> seen_;
};

FilesystemServer make(const Root& r, FilesystemConfig cfg = {}) {
    return FilesystemServer(r.dir, cfg, TEST_DATA_DIR, 0);
}

}  // namespace

SCENARIO("gh#201: read_file numbers every line and pages long files",
         "[filesystem][contract][gh201][gh203][2.14.0]") {
    Root r;
    std::string ten;
    for (int i = 1; i <= 10; ++i) { ten += "line " + std::to_string(i) + "\n"; }
    r.write("ten.txt", ten);
    r.write(".gitignore", "secret.txt\n");
    r.write("secret.txt", "x\n");
    r.write("wide.txt", std::string(64, 'w') + "\n");
    fs::create_directories(r.dir / "adir");
    FilesystemConfig cfg;
    cfg.max_read_bytes = 40;  // ~5 short lines per window
    auto server = make(r, cfg);
    ContractRun run(server, "read_file");

    THEN("each line carries its number, and the first window says where to resume") {
        auto out = run.call({{"path", "ten.txt"}});
        CHECK(out["path"] == "ten.txt");
        CHECK(out["total"] == 10);
        CHECK(out["lines"][0] == json{{"line", 1}, {"content", "line 1"}});
        REQUIRE(out["truncated"] == true);
        const long next = out["next_offset"];
        CHECK(next == 1 + static_cast<long>(out["lines"].size()));
        AND_THEN("the next window starts exactly there") {
            auto more = run.call({{"path", "ten.txt"}, {"offset", next}});
            CHECK(more["lines"][0]["line"] == next);
        }
    }
    THEN("offset/limit select a window") {
        auto out = run.call({{"path", "ten.txt"}, {"offset", 9}, {"limit", 5}});
        REQUIRE(out["lines"].size() == 2);
        CHECK(out["lines"][1] == json{{"line", 10}, {"content", "line 10"}});
        CHECK(out["truncated"] == false);
        CHECK_FALSE(out.contains("next_offset"));
    }
    THEN("every declared error is reachable and nothing undeclared is emitted") {
        run.call({{"path", "missing.txt"}});
        run.call({{"path", "adir"}});
        run.call({{"path", "secret.txt"}});
        run.call({{"path", "wide.txt"}});            // a line larger than the budget
        run.call({{"path", "ten.txt"}, {"offset", 0}});
        run.require_codes_exercised();
    }
}

SCENARIO("gh#202: grep pages past its cap and can search one file",
         "[filesystem][contract][gh173][gh202][gh203][2.14.0]") {
    Root r;
    std::string body;
    for (int i = 1; i <= 7; ++i) { body += "hit " + std::to_string(i) + "\n"; }
    r.write("src/a.txt", body);
    r.write("src/b.txt", "hit b\n");
    r.write("blob.bin", std::string("hit\0hit", 7));
    auto server = make(r);
    ContractRun run(server, "grep");

    THEN("a full page says more exist and where to continue") {
        auto out = run.call({{"pattern", "hit"}, {"path", "src/a.txt"}, {"limit", 3}});
        REQUIRE(out["matches"].size() == 3);
        CHECK(out["matches"][0] == json{{"path", "src/a.txt"}, {"line", 1}, {"content", "hit 1"}});
        CHECK(out["truncated"] == true);
        CHECK(out["reason"] == "limit");
        CHECK(out["next_offset"] == 3);
        AND_THEN("the continuation returns the rest and no more") {
            auto rest = run.call({{"pattern", "hit"}, {"path", "src/a.txt"}, {"offset", 3}});
            REQUIRE(rest["matches"].size() == 4);
            CHECK(rest["matches"][0]["line"] == 4);
            CHECK(rest["truncated"] == false);
        }
    }
    THEN("a binary file is skipped, as the description says") {
        auto out = run.call({{"pattern", "hit"}, {"glob", "*.bin"}});
        CHECK(out["matches"].empty());
    }
    THEN("every declared error is reachable") {
        run.call({{"pattern", "[unclosed"}});
        run.call({{"pattern", "hit"}, {"path", "nope"}});
        run.call({{"pattern", "hit"}, {"limit", 0}});
        run.require_codes_exercised();
    }
}

SCENARIO("gh#203: edit_file inserts AFTER the line, and line 0 is the start",
         "[filesystem][contract][gh203][2.14.0]") {
    Root r;
    r.write("f.txt", "a\nb\nc");  // no final newline
    auto server = make(r);
    server.execute("read_file", json{{"path", "f.txt"}}.dump());
    ContractRun run(server, "edit_file");

    WHEN("insert_line is 0") {
        auto out = run.call({{"path", "f.txt"}, {"insert_line", 0}, {"new_string", "top"}});
        THEN("the text lands at the start — it used to be a silent no-op") {
            CHECK(out["inserted_after"] == 0);
            CHECK(r.read("f.txt") == "top\na\nb\nc");
        }
    }
    WHEN("insert_line is the last line of a file with no final newline") {
        run.call({{"path", "f.txt"}, {"insert_line", 3}, {"new_string", "end"}});
        THEN("it follows that line, and the file keeps its ending") {
            CHECK(r.read("f.txt") == "a\nb\nc\nend");
        }
    }
    WHEN("replacing") {
        auto out = run.call({{"path", "f.txt"}, {"old_string", "b"}, {"new_string", "B"}});
        THEN("the result names the mode and the count") {
            CHECK(out["mode"] == "str_replace");
            CHECK(out["replacements"] == 1);
            CHECK(out["path"] == "f.txt");
        }
    }
    THEN("every declared error is reachable and leaves the file untouched") {
        r.write("g.txt", "x x\n");
        run.call({{"path", "g.txt"}, {"old_string", "x"}, {"new_string", "y"}});  // unread
        server.execute("read_file", json{{"path", "g.txt"}}.dump());
        run.call({{"path", "g.txt"}, {"old_string", "zzz"}, {"new_string", "y"}});
        run.call({{"path", "g.txt"}, {"old_string", "x"}, {"new_string", "y"}});
        run.call({{"path", "g.txt"}, {"insert_line", 5}, {"new_string", "y"}});
        run.call({{"path", "g.txt"}, {"new_string", "y"}});
        run.require_codes_exercised();
        CHECK(r.read("g.txt") == "x x\n");
    }
}

SCENARIO("gh#203: glob, list_directory and write_file emit their declared shapes",
         "[filesystem][contract][gh203][2.14.0]") {
    Root r;
    for (int i = 0; i < 5; ++i) { r.write("d/f" + std::to_string(i) + ".txt", "x"); }
    auto server = make(r);

    THEN("glob pages root-relative paths") {
        ContractRun run(server, "glob");
        auto out = run.call({{"pattern", "**/*.txt"}, {"limit", 2}});
        CHECK(out["paths"].size() == 2);
        CHECK(out["paths"][0].get<std::string>().rfind("d/", 0) == 0);
        CHECK(out["reason"] == "limit");
        run.call({{"pattern", "*"}, {"limit", 99999}});
        run.require_codes_exercised();
    }
    THEN("list_directory pages a sorted listing with relative names") {
        ContractRun run(server, "list_directory");
        auto out = run.call({{"recursive", true}, {"limit", 3}});
        CHECK(out["path"] == ".");
        REQUIRE(out["entries"].size() == 3);
        CHECK(out["entries"][0]["name"] == "d");
        CHECK(out["entries"][1]["name"] == "d/f0.txt");
        CHECK(out["next_offset"] == 3);
        run.call({{"path", "d/f0.txt"}});
        run.call({{"offset", -1}});
        run.require_codes_exercised();
    }
    THEN("write_file reports the root-relative path") {
        ContractRun run(server, "write_file");
        auto out = run.call({{"path", "new/x.txt"}, {"content", "hi"}});
        CHECK(out["path"] == "new/x.txt");
        CHECK(out["bytes_written"] == 2);
        run.call({{"path", "d/f0.txt"}, {"content", "y"}});  // exists, unread
        run.require_codes_exercised();
    }
}

SCENARIO("gh#203: each filesystem description's Response format is generated "
         "from its outputSchema", "[filesystem][contract][gh203][2.14.0]") {
    for (const char* tool : {"read_file", "write_file", "edit_file", "glob",
                             "grep", "list_directory"}) {
        auto def = entropic::load_tool_definition(
            tool, "filesystem", std::string(TEST_DATA_DIR) + "/tools");
        INFO(tool << ":\n" << def.description);
        CHECK(def.description.find("**Response format** (JSON):")
              != std::string::npos);
        CHECK(def.description.find(entropic::render_response_format(def.output_schema))
              != std::string::npos);
    }
}
