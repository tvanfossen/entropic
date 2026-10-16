// SPDX-License-Identifier: Apache-2.0
/**
 * @file git_injection_test.cpp
 * @brief gh#211 (v2.14.4): no git tool argument — and no repository path —
 *        ever reaches a shell.
 *
 * run_git built `git -C <repo_dir> <args>` and handed it to popen(), with no
 * quoting: a model-supplied `file`, `target`, `files`, `create` or commit
 * `message` ran arbitrary commands as the user (the git server is on by
 * default). Each probe here carries `; touch '<marker>'` (or `$(touch ...)`)
 * — harmless, and observable: the marker must never appear. Option-shaped
 * values must not become options either (`--output=<path>` would write a
 * file). And real values must still work, literally.
 *
 * @version 2.14.4
 */

#include <entropic/mcp/servers/git.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using entropic::GitServer;

namespace {

/// @brief A scratch git repository (and a sibling dir for markers).
struct Repo {
    fs::path base;
    fs::path root;
    explicit Repo(const std::string& name = "repo") {
        base = fs::temp_directory_path() / ("entropic_gitinj_" + std::to_string(::getpid()));
        fs::remove_all(base);
        root = base / name;
        fs::create_directories(root);
        const std::string q = "'" + root.string() + "'";
        std::system(("git -C " + q + " init -q && git -C " + q
                     + " config user.email t@t && git -C " + q
                     + " config user.name T").c_str());
        std::ofstream(root / "a.txt") << "one\n";
        std::system(("git -C " + q + " add a.txt && git -C " + q
                     + " commit -qm init").c_str());
    }
    ~Repo() { std::error_code ec; fs::remove_all(base, ec); }
    Repo(const Repo&) = delete;
    Repo& operator=(const Repo&) = delete;
    fs::path marker() const { return base / "PWNED"; }
};

/// @brief Run one git tool; return its result object.
json run(GitServer& s, const std::string& tool, const json& args) {
    auto env = json::parse(s.execute(tool, args.dump()));
    return json::parse(env.at("result").get<std::string>());
}

}  // namespace

SCENARIO("gh#211: a shell metacharacter in any git argument runs nothing",
         "[git][security][gh211][2.14.4]") {
    Repo r;
    GitServer git(r.root, TEST_DATA_DIR);
    const std::string inject = "x; touch '" + r.marker().string() + "'";

    for (const auto& [tool, args] : std::initializer_list<std::pair<const char*, json>>{
             {"diff", {{"file", inject}}},
             {"checkout", {{"target", inject}}},
             {"add", {{"files", inject}}},
             {"reset", {{"files", inject}}},
             {"branch", {{"name", inject}}},
             {"commit", {{"message", "$(touch '" + r.marker().string() + "')"}}},
         }) {
        INFO(tool << " " << args.dump());
        run(git, tool, args);
        CHECK_FALSE(fs::exists(r.marker()));
    }
}

SCENARIO("gh#211: the repository path never reaches a shell either",
         "[git][security][gh211][2.14.4]") {
    // Under `entropic serve` the root is a caller-supplied workspace path.
    Repo r("repo; touch PWNED_ROOT");
    GitServer git(r.root, TEST_DATA_DIR);
    auto out = run(git, "status", json::object());
    INFO(out.dump());
    CHECK(out["exit_code"] == 0);
    CHECK_FALSE(fs::exists(fs::current_path() / "PWNED_ROOT"));
    CHECK_FALSE(fs::exists(r.base / "PWNED_ROOT"));
}

SCENARIO("gh#211: option-shaped values are not options",
         "[git][security][gh211][2.14.4]") {
    Repo r;
    GitServer git(r.root, TEST_DATA_DIR);
    const auto out_path = r.base / "written-by-diff";
    WHEN("diff's file is --output=<path>") {
        run(git, "diff", {{"file", "--output=" + out_path.string()}});
        THEN("it is a pathspec, not an option — nothing is written") {
            CHECK_FALSE(fs::exists(out_path));
        }
    }
    WHEN("checkout's target or branch's name starts with '-'") {
        auto co = run(git, "checkout", {{"target", "--orphan=evil"}});
        auto br = run(git, "branch", {{"name", "-D"}});
        THEN("both are refused as invalid arguments") {
            INFO(co.dump() << "\n" << br.dump());
            CHECK(co.value("error", "") == "invalid_args");
            CHECK(br.value("error", "") == "invalid_args");
        }
    }
}

SCENARIO("gh#211: ordinary values still work, taken literally",
         "[git][security][gh211][2.14.4]") {
    Repo r;
    GitServer git(r.root, TEST_DATA_DIR);
    std::ofstream(r.root / "b.txt") << "two\n";
    std::ofstream(r.root / "c.txt") << "three\n";
    // `files` is space-separated by contract (add.json) — split, no shell.
    CHECK(run(git, "add", {{"files", "b.txt c.txt"}})["exit_code"] == 0);
    const std::string msg = "Fix \"quoted\" $(not run) `nor this`";
    CHECK(run(git, "commit", {{"message", msg}})["exit_code"] == 0);
    auto log = run(git, "log", {{"count", 1}});
    INFO(log.dump());
    CHECK(log["output"].get<std::string>().find(msg) != std::string::npos);
    CHECK(run(git, "branch", {{"name", "feature/x"}})["exit_code"] == 0);
}
