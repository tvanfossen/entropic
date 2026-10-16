// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_contract_gate_test.cpp
 * @brief gh#203 (v2.14.0): the gate that keeps every bundled tool's
 *        advertised response shape equal to the shape its handler emits.
 *
 * Decision 4 of the v2.14.0 plan: no undocumented behaviour in a tool's
 * output. This test fails when:
 *   - a `data/tools` descriptor has no `outputSchema`;
 *   - a descriptor hand-writes a "Response format" block (it is generated);
 *   - a descriptor exists that no fixture here (or filesystem's own
 *     contract test) drives — an orphan like `todo_write.json` was;
 *   - a handler emits a key, shape or error code its schema does not
 *     declare, or a schema declares an error code nothing emits.
 *
 * Filesystem tools are driven by filesystem_contract_test.cpp; everything
 * else is driven here.
 *
 * @version 2.14.0
 */

#include <entropic/mcp/servers/bash.h>
#include <entropic/mcp/servers/diagnostics.h>
#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/servers/git.h>
#include <entropic/mcp/servers/web.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/types/run_scope.h>

#include "../output_contract.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const fs::path kTools = fs::path(TEST_DATA_DIR) / "tools";

/// @brief Tools whose fixtures live in filesystem_contract_test.cpp.
const std::set<std::string> kFilesystemCovered = {
    "filesystem/read_file", "filesystem/write_file", "filesystem/edit_file",
    "filesystem/glob", "filesystem/grep", "filesystem/list_directory"};

/// @brief Every bundled descriptor, as "<server>/<tool>".
std::set<std::string> bundled_tools() {
    std::set<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(kTools)) {
        if (e.path().extension() == ".json") {
            out.insert(e.path().parent_path().filename().string() + "/"
                       + e.path().stem().string());
        }
    }
    return out;
}

/// @brief A descriptor file, parsed.
json descriptor(const std::string& key) {
    std::ifstream in(kTools / (key + ".json"));
    return json::parse(in);
}

/// @brief Every error code a schema declares, in any branch.
std::set<std::string> declared_codes(const json& schema) {
    std::set<std::string> codes;
    for (const auto& branch : schema.value("oneOf", json::array({schema}))) {
        const auto props = branch.value("properties", json::object());
        if (props.contains("error")) {
            for (const auto& c : props["error"].value("enum", json::array())) {
                codes.insert(c.get<std::string>());
            }
        }
    }
    return codes;
}

/// @brief Drives fixtures and records which tools/codes they reached.
class Gate {
public:
    /// @brief Run `tool` on `server`; the result must satisfy the contract.
    json run(entropic::MCPServerBase& server, const std::string& key,
             const json& args) {
        const auto tool = key.substr(key.find('/') + 1);
        const auto schema = descriptor(key).at("outputSchema");
        auto env = json::parse(server.execute(tool, args.dump()));
        const auto text = env.at("result").get<std::string>();
        INFO(key << " " << args.dump() << " -> " << text);
        auto out = json::parse(text, nullptr, false);
        REQUIRE_FALSE(out.is_discarded());
        const auto why = entropic::test::contract_violation(schema, out);
        INFO(why);
        CHECK(why.empty());
        exercised_.insert(key);
        if (out.is_object() && out.contains("error")) {
            seen_[key].insert(out["error"].get<std::string>());
        }
        return out;
    }

    /// @brief Every non-filesystem descriptor was driven, and for each one
    ///        the codes produced equal the codes declared.
    void require_complete() const {
        for (const auto& key : bundled_tools()) {
            if (kFilesystemCovered.count(key) != 0) { continue; }
            INFO(key << " has no fixture in this gate");
            CHECK(exercised_.count(key) == 1);
            const auto it = seen_.find(key);
            const auto seen = it == seen_.end() ? std::set<std::string>{} : it->second;
            INFO(key << ": declared vs produced error codes differ");
            CHECK(seen == declared_codes(descriptor(key).at("outputSchema")));
        }
    }

private:
    std::set<std::string> exercised_;
    std::map<std::string, std::set<std::string>> seen_;
};

// ── a minimal state provider ──────────────────────────────────
char* p_obj(void*) { return strdup(R"({"a":1})"); }
char* p_arr(void*) { return strdup(R"([{"name":"lead"}])"); }
char* p_hist(int, void*) {
    return strdup(R"([{"role":"user","content_preview":"hi","token_count_est":1}])");
}
char* p_docs(const char*, void*) { return strdup("docs text"); }
char* p_search(const char*, int, void*) {
    return strdup(R"([{"delegation_id":"d1","target_tier":"eng","summary":"s","completed_at":"t"}])");
}
entropic_state_provider_t provider() {
    entropic_state_provider_t p{};
    p.get_config = p_obj;
    p.get_identities = p_arr;
    p.get_tools = p_arr;
    p.get_history = p_hist;
    p.get_state = p_obj;
    p.get_metrics = p_obj;
    p.get_docs = p_docs;
    p.search_delegations = p_search;
    return p;
}

/// @brief A scratch directory, removed on scope exit.
struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path()
            / ("entropic_gate_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

void drive_bash(Gate& g, const fs::path& dir) {
    entropic::BashServer bash(dir, TEST_DATA_DIR, 1);
    g.run(bash, "bash/execute", {{"command", "echo hi"}});
    g.run(bash, "bash/execute", {{"command", "sleep 5"}});  // 1 s limit
    std::atomic<bool> cancelled{true};
    entropic::RunCancelScope scope(&cancelled);
    g.run(bash, "bash/execute", {{"command", "sleep 5"}});
}

void drive_git(Gate& g, const fs::path& dir) {
    const auto repo = dir / "repo";
    fs::create_directories(repo);
    REQUIRE(std::system(("git -C " + repo.string() + " init -q").c_str()) == 0);
    std::ofstream(repo / "f.txt") << "x\n";
    entropic::GitServer git(repo, TEST_DATA_DIR);
    g.run(git, "git/status", json::object());
    g.run(git, "git/add", {{"files", "."}});
    g.run(git, "git/diff", {{"staged", true}});
    g.run(git, "git/commit", {{"message", "m"}});   // may fail: no identity
    g.run(git, "git/log", {{"count", 1}});
    g.run(git, "git/branch", json::object());
    g.run(git, "git/checkout", {{"target", "no-such-branch"}});
    g.run(git, "git/checkout", {{"target", "-x"}});   // gh#211: refused, not an option
    g.run(git, "git/branch", {{"name", "-x"}});
    g.run(git, "git/reset", json::object());
}

void drive_entropic_control(Gate& g, entropic::EntropicServer& s) {
    g.run(s, "entropic/todo", {{"action", "add"}, {"content", "a"}});
    g.run(s, "entropic/todo", {{"action", "update"}, {"index", 9}});
    g.run(s, "entropic/todo", {{"action", "bogus"}});
    g.run(s, "entropic/delegate", {{"target", "lead"}, {"task", "t"}});
    g.run(s, "entropic/delegate", {{"target", "eng"}, {"task", "t"}});
    const json ctx = json::array({{{"path", "a.cpp"}}});
    g.run(s, "entropic/pipeline", {{"stages", {"lead", "eng"}}, {"task", "t"}, {"context", ctx}});
    g.run(s, "entropic/pipeline", {{"stages", {"lead"}}, {"task", "t"}});
    g.run(s, "entropic/pipeline", {{"stages", {"lead", "zzz"}}, {"task", "t"}});
    g.run(s, "entropic/pipeline", {{"stages", {"lead", "eng"}}, {"task", "t"}});
    g.run(s, "entropic/complete", {{"summary", "s"}});
    g.run(s, "entropic/complete", {{"summary", "s"}, {"coverage_gap", true}});
    g.run(s, "entropic/prune_context", json::object());
    g.run(s, "entropic/resume_delegation", {{"task", "t"}, {"target", "eng"}});
    g.run(s, "entropic/resume_delegation", json::object());
}

void drive_entropic_introspection(Gate& g, entropic::EntropicServer& bare,
                                  entropic::EntropicServer& wired) {
    for (const char* key : {"entropic/diagnose", "entropic/inspect",
                            "entropic/context_inspect"}) {
        g.run(bare, key, json::object());
        g.run(wired, key, json::object());
    }
    g.run(wired, "entropic/diagnose", {{"include_docs", true}});
    g.run(wired, "entropic/inspect", {{"target", "docs"}});
    g.run(wired, "entropic/inspect", {{"target", "tool"}, {"key", "lead"}});
    g.run(wired, "entropic/inspect", {{"target", "tool"}, {"key", "nope"}});
    g.run(wired, "entropic/inspect", {{"target", "bogus"}});
    g.run(bare, "entropic/followup", {{"query", "q"}});
    g.run(wired, "entropic/followup", {{"query", "q"}});
    g.run(wired, "entropic/followup", json::object());
}

}  // namespace

SCENARIO("gh#203: every bundled descriptor declares an outputSchema and "
         "generates its Response format", "[contract][gate][gh203][2.14.0]") {
    for (const auto& key : bundled_tools()) {
        INFO(key);
        const auto d = descriptor(key);
        REQUIRE(d.contains("outputSchema"));
        CHECK(d["outputSchema"].is_object());
        std::string authored = d.value("description", "");
        for (auto& c : authored) { c = static_cast<char>(std::tolower(c)); }
        CHECK(authored.find("response format") == std::string::npos);
        const auto server = key.substr(0, key.find('/'));
        const auto def = entropic::load_tool_definition(
            key.substr(key.find('/') + 1), server, kTools.string());
        CHECK(def.description == d.value("description", "")
              + entropic::render_response_format(def.output_schema));
    }
}

SCENARIO("gh#203: every bundled tool emits only what its outputSchema declares",
         "[contract][gate][gh203][2.14.0]") {
    Scratch scratch;
    Gate gate;
    drive_bash(gate, scratch.dir);
    drive_git(gate, scratch.dir);
    entropic::EntropicServer control({"lead", "eng"}, TEST_DATA_DIR, {"eng"});
    drive_entropic_control(gate, control);
    entropic::EntropicServer bare({"lead", "eng"}, TEST_DATA_DIR);
    entropic::EntropicServer wired({"lead", "eng"}, TEST_DATA_DIR);
    auto p = provider();
    wired.set_state_provider(p);
    drive_entropic_introspection(gate, bare, wired);
    entropic::DiagnosticsServer diag(scratch.dir, TEST_DATA_DIR);
    gate.run(diag, "diagnostics/diagnostics", {{"file_path", "x"}});
    gate.run(diag, "diagnostics/check_errors", {{"file_path", "x"}});
    entropic::WebServer web(TEST_DATA_DIR);
    gate.run(web, "web/web_fetch", {{"url", "http://example.invalid"}});
    gate.run(web, "web/web_search", {{"query", "q"}});
    gate.require_complete();
}

SCENARIO("gh#203: the rendered Response format states every key, optional "
         "marker, error code and field note — and nothing else",
         "[contract][render][gh203][2.14.0]") {
    const std::string schema = R"({"oneOf":[
        {"title":"success","type":"object","required":["items","truncated"],
         "properties":{"items":{"type":"array","items":{"type":"string"}},
                       "truncated":{"type":"boolean"},
                       "next_offset":{"type":"integer","description":"pass as offset"}}},
        {"title":"error","type":"object","required":["error","message"],
         "properties":{"error":{"enum":["a","b"]},"message":{"type":"string"}}}]})";
    CHECK(entropic::render_response_format(schema) ==
        "\n\n**Response format** (JSON):\n"
        "- success: {\"items\": [string, ...], \"truncated\": boolean, \"next_offset\"?: integer}\n"
        "- error: {\"error\": \"a\" | \"b\", \"message\": string}\n"
        "- `next_offset`: pass as offset\n"
        "Other failures: plain text `Error: ...`.\n");
    CHECK(entropic::render_response_format("").empty());
    CHECK(entropic::render_response_format("not json").empty());
}
