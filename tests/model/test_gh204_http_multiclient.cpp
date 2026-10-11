// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh204_http_multiclient.cpp
 * @brief gh#204/gh#205: two MCP clients over Streamable HTTP, one engine —
 *        different sessions, workspaces and tiers, at the same time.
 *
 * The engagement test v2.14.0's serving work owed. Each client is a plain
 * HTTP client holding the bearer token from the token file, as a plugin
 * would be. Every `ask` names its own session, its own repository (an
 * absolute path, so the bridge creates the workspace) and its own tier.
 * What each answer must show is only reachable through its own triple:
 *   - the tier's identity makes it open with its tag (ALPHA / BRAVO);
 *   - the repository's `.entropic/ENTROPIC.md` (workspace_app_context)
 *     names its codename (Marigold / Saffron) — no tool call can find it.
 *
 * Engagement, read from the LOG rather than inferred: the bridge logs
 * "[turn] session '<key>' admitted after N ms". The two first-round asks
 * overlap in wall time; if turns were still serialized bridge-wide (the
 * pre-v2.14.0 queue), the second session would wait out most of the
 * first's turn. Each session's admission wait must be a small fraction of
 * the OTHER request's duration.
 *
 * Multi-turn: each client asks a second time in the same session, so the
 * workspace binding and the conversation carry across HTTP requests.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: inv test --model --filter gh204
 *
 * @version 2.14.0
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

/// @brief Case-insensitive substring test.
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief A port nothing is listening on right now.
int free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/// @brief A repository whose only distinguishing content is its codename.
fs::path make_repo(const std::string& slot, const std::string& codename) {
    auto dir = fs::temp_directory_path()
        / ("entropic_gh204_" + slot + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".entropic");
    std::ofstream(dir / ".entropic" / "ENTROPIC.md")
        << "This project's internal codename is " << codename
        << ". When asked for the codename, give exactly that word.\n";
    return dir;
}

/// @brief One client's view of one ask.
struct Ask {
    std::string text;          ///< The tool result's text
    Clock::time_point start;   ///< Request sent
    Clock::time_point end;     ///< Response complete
    int status = 0;            ///< HTTP status
};

/// @brief POST one entropic.ask and pull the result text out of the SSE.
Ask ask(int port, const std::string& token, int id, const json& arguments) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(600, 0);
    const json req{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                   {"params", {{"name", "entropic.ask"}, {"arguments", arguments}}}};
    Ask out;
    out.start = Clock::now();
    auto r = cli.Post("/mcp", {{"Authorization", "Bearer " + token}},
                      req.dump(), "application/json");
    out.end = Clock::now();
    if (!r) { return out; }
    out.status = r->status;
    std::istringstream sse(r->body);
    for (std::string line; std::getline(sse, line);) {
        if (line.rfind("data: ", 0) != 0) { continue; }
        auto msg = json::parse(line.substr(6), nullptr, false);
        if (msg.is_object() && msg.value("id", -1) == id && msg.contains("result")) {
            for (const auto& c : msg["result"].value("content", json::array())) {
                out.text += c.value("text", "");
            }
        }
    }
    return out;
}

/// @brief Every "[turn] session '<key>' admitted after N ms" in the log.
std::map<std::string, std::vector<long>> admissions(const std::string& test_name) {
    if (g_file_sink) { g_file_sink->flush(); }
    std::ifstream in((LOG_DIR / (test_name + ".log")).string());
    const std::regex re(R"(\[turn\] session '([^']*)' admitted after (\d+) ms)");
    std::map<std::string, std::vector<long>> out;
    std::smatch m;
    for (std::string line; std::getline(in, line);) {
        if (std::regex_search(line, m, re)) { out[m[1]].push_back(std::stol(m[2])); }
    }
    return out;
}

long ms(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

}  // namespace

SCENARIO("gh#204/gh#205: two HTTP clients, different session, workspace and "
         "tier, run concurrently on one engine", "[model][gh204][gh205][serve]") {
    GIVEN("one engine serving MCP over HTTP, two tiers, two repositories") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        const auto repo_a = make_repo("a", "Marigold");
        const auto repo_b = make_repo("b", "Saffron");
        const int port = free_port();

        entropic::test::facade::FacadeProject project("gh204_http_multiclient");
        project.extra_config = "workspace_app_context: .entropic/ENTROPIC.md\n";
        project.extra_mcp_config = "  external:\n    enabled: true\n    http_port: "
                                   + std::to_string(port) + "\n";
        auto tier = [](const std::string& name, const std::string& tag) {
            entropic::test::facade::TierSpec t;
            t.name = name;
            t.gguf_key = "gemma4_e2b_qat";  // both tiers share one backend
            t.adapter = "gemma4";
            t.identity_body = "You are a terse assistant. Begin EVERY answer "
                              "with the word " + tag + ", then answer briefly.";
            t.context_length = 4096;
            t.allowed_tools = {"entropic.complete"};
            // The property is context + concurrency, not completion
            // discipline. A derived explicit_completion made E2B spend its
            // turn on unanswerable "end with entropic.complete" nudges and
            // the bridge's final text collapsed to the tag (first gate run).
            t.explicit_completion = false;
            return t;
        };
        auto* h = project.setup({tier("lead", "ALPHA"), tier("eng", "BRAVO")});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);
        start_test_log("gh204_http_multiclient");

        const auto token_file = fs::path(std::getenv("HOME"))
            / ".entropic" / "serve" / (std::to_string(port) + ".token");
        std::ifstream tf(token_file);
        const std::string token((std::istreambuf_iterator<char>(tf)), {});
        REQUIRE(token.size() == 64);

        auto args = [&](const std::string& session, const fs::path& repo,
                        const std::string& tier_name, const std::string& prompt) {
            return json{{"prompt", prompt}, {"session", session},
                        {"workspace", repo.string()}, {"tier", tier_name}};
        };
        const std::string first = "Write two sentences about the sea, then on a "
                                  "new line state this project's codename.";
        const std::string again = "What is this project's codename? One word "
                                  "after your opening word.";

        WHEN("both clients ask at the same time, then ask again") {
            Ask a1, b1, a2, b2;
            std::thread ta([&] {
                a1 = ask(port, token, 1, args("sess-a", repo_a, "lead", first));
                a2 = ask(port, token, 2, args("sess-a", repo_a, "lead", again));
            });
            std::thread tb([&] {
                b1 = ask(port, token, 3, args("sess-b", repo_b, "eng", first));
                b2 = ask(port, token, 4, args("sess-b", repo_b, "eng", again));
            });
            ta.join();
            tb.join();
            INFO("A1 (" << ms(a1.end - a1.start) << " ms): " << a1.text);
            INFO("A2: " << a2.text);
            INFO("B1 (" << ms(b1.end - b1.start) << " ms): " << b1.text);
            INFO("B2: " << b2.text);

            THEN("every ask was served") {
                CHECK(a1.status == 200);
                CHECK(b1.status == 200);
                CHECK(a2.status == 200);
                CHECK(b2.status == 200);
            }
            THEN("each answer carries its own tier's tag and its own repository's codename") {
                CHECK(has_ci(a1.text, "alpha"));
                CHECK(has_ci(b1.text, "bravo"));
                CHECK(has_ci(a1.text, "marigold"));
                CHECK(has_ci(b1.text, "saffron"));
                CHECK(has_ci(a2.text, "marigold"));   // binding + conversation carried
                CHECK(has_ci(b2.text, "saffron"));
                CHECK_FALSE(has_ci(a1.text + a2.text, "saffron"));
                CHECK_FALSE(has_ci(b1.text + b2.text, "marigold"));
            }
            THEN("the two first-round turns ran together — from the bridge's own log") {
                CHECK(a1.start < b1.end);
                CHECK(b1.start < a1.end);   // the requests overlapped
                const auto adm = admissions("gh204_http_multiclient");
                REQUIRE(adm.count("sess-a") == 1);
                REQUIRE(adm.count("sess-b") == 1);
                REQUIRE_FALSE(adm.at("sess-a").empty());
                REQUIRE_FALSE(adm.at("sess-b").empty());
                const long wait_a = adm.at("sess-a").front();
                const long wait_b = adm.at("sess-b").front();
                INFO("admitted after: sess-a " << wait_a << " ms, sess-b " << wait_b << " ms");
                // Serialized, one of these would be most of the other's turn.
                CHECK(wait_a * 4 < ms(b1.end - b1.start));
                CHECK(wait_b * 4 < ms(a1.end - a1.start));
            }
        }
        fs::remove_all(repo_a);
        fs::remove_all(repo_b);
    }
}
