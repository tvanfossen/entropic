// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file builtin_server_sharing_test.cpp
 * @brief gh#158 (v2.13.0): the built-in MCP servers are shared between
 *        concurrent sessions, and must be safe to share.
 *
 * `concurrent_sessions` defaults TRUE, so two UNBOUND sessions run against
 * the handle's one DEFAULT server set and can call the same tool at the
 * same moment. The first gh#158 audit pass covered engine state and the
 * external stdio transports; these scenarios cover the in-process servers'
 * OWN mutable state — the read-before-write tracker, and the working
 * directory a delegation sandbox re-points (gh#160).
 *
 * A fourth set of scenarios (gh#158, fourth pass) proves the tracker and
 * the todo list are per SESSION rather than per server: another session's
 * read unlocks nothing, a delegated child's read unlocks its parent's
 * write (it runs under the parent's key), and ending a conversation —
 * drop, clear, restore — releases exactly that session's entries.
 *
 * Every dispatch goes through the handle's real `ToolExecutor`, which is
 * the path a run takes; the sandbox swap is the facade's PRODUCTION
 * callback (`swap_session_tool_dir`) driven through a real `ScopedSandbox`,
 * exactly as `DelegationManager` drives it. The race half of each claim is
 * proven by the
 * `tsan` preset (a report fails the test binary); the behavioural half is
 * asserted here, so the CPU lane catches a wrong answer too.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/core/delegation.h>
#include <entropic/core/sandbox.h>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: tool_executor, swap_session_tool_dir

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

/**
 * @brief A temporary directory removed on destruction.
 * @internal
 * @version 2.13.0
 */
struct TempDir {
    fs::path path;  ///< Directory

    /** @brief Create a fresh directory. @internal @version 2.13.0 */
    explicit TempDir(const std::string& tag)
        : path(fs::temp_directory_path()
               / ("entropic_share_" + tag + "_"
                  + std::to_string(::getpid()))) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    /** @brief Remove the tree. @internal @version 2.13.0 */
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /**
     * @brief Write one file under the directory.
     * @param rel Relative name.
     * @param body Content.
     * @internal
     * @version 2.13.0
     */
    void put(const std::string& rel, const std::string& body) const {
        std::ofstream(path / rel) << body;
    }
};

/**
 * @brief RAII handle rooted at `root`, tools auto-approved.
 *
 * auto_approve so the ToolExecutor's approval gate passes without a
 * callback — the property under test is where a call LANDS, not whether
 * it was approved.
 *
 * @internal
 * @version 2.13.0
 */
struct Handle {
    entropic_handle_t h = nullptr;  ///< Handle under test
    bool ok = false;                ///< Whether configure succeeded

    /** @brief Create + configure. @internal @version 2.13.0 */
    explicit Handle(const fs::path& root) {
        std::string json =
            R"({"log_level":"WARN","permissions":{"auto_approve":true},)"
            R"("mcp":{"working_dir":")" + root.string() + R"("}})";
        entropic_create(&h);
        if (h != nullptr) {
            ok = entropic_configure(h, json.c_str()) == ENTROPIC_OK;
        }
    }
    /** @brief Destroy. @internal @version 2.13.0 */
    ~Handle() { entropic_destroy(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

/**
 * @brief Build one tool call with a JSON argument object.
 * @param id Call id.
 * @param name Fully-qualified tool name.
 * @param args Argument object.
 * @return The call, with both argument forms populated.
 * @internal
 * @version 2.13.0
 */
entropic::ToolCall make_call(const std::string& id, const std::string& name,
                             const nlohmann::json& args) {
    entropic::ToolCall call;
    call.id = id;
    call.name = name;
    for (auto it = args.begin(); it != args.end(); ++it) {
        call.arguments[it.key()] = it.value().is_string()
            ? it.value().get<std::string>() : it.value().dump();
    }
    call.arguments_json = args.dump();
    return call;
}

/**
 * @brief Dispatch a batch as session `key` through the real executor.
 * @param h Configured handle.
 * @param key Session key the calls belong to.
 * @param calls Batch.
 * @return Result text per call, in order.
 * @internal
 * @version 2.13.0
 */
std::vector<std::string> dispatch(entropic_handle_t h, const std::string& key,
                                  const std::vector<entropic::ToolCall>& calls) {
    entropic::LoopContext ctx;
    ctx.session_key = key;
    auto msgs = h->tool_executor->process_tool_calls(ctx, calls);
    std::vector<std::string> out;
    for (const auto& m : msgs) { out.push_back(m.content); }
    return out;
}

/**
 * @brief The joined result text of one batch, for marker matching.
 * @param results Per-call result texts.
 * @return All of them, newline-separated.
 * @internal
 * @version 2.13.0
 */
std::string joined(const std::vector<std::string>& results) {
    std::string all;
    for (const auto& r : results) { all += r + "\n"; }
    return all;
}

/**
 * @brief read_file + `bash cat` of one relative path — two servers, two
 *        root fields (FilesystemServer::root_dir_, BashServer::working_dir_).
 * @param id Call-id stem (distinct per batch — a repeat keys as duplicate).
 * @param rel Path relative to the resolving servers' root.
 * @return The two-call batch.
 * @internal
 * @version 2.13.0
 */
std::vector<entropic::ToolCall> read_both(const std::string& id,
                                          const std::string& rel) {
    return {make_call(id + "-fs", "filesystem.read_file", {{"path", rel}}),
            make_call(id + "-sh", "bash.execute",
                      {{"command", "cat " + rel}})};
}

/**
 * @brief Whole contents of a file on disk.
 * @param p Path.
 * @return Its bytes, or "" when unreadable.
 * @internal
 * @version 2.13.0
 */
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @brief One call's result text, as session `key`.
 * @param h Configured handle.
 * @param key Session key.
 * @param call The call.
 * @return Its result text ("<no result>" when none came back).
 * @internal
 * @version 2.13.0
 */
std::string one(entropic_handle_t h, const std::string& key,
                const entropic::ToolCall& call) {
    auto res = dispatch(h, key, {call});
    return res.empty() ? "<no result>" : res.front();
}

/**
 * @brief read_file of `rel`.
 * @param id Call id (distinct per call).
 * @param rel Root-relative path.
 * @return The call.
 * @internal
 * @version 2.13.0
 */
entropic::ToolCall read_call(const std::string& id, const std::string& rel) {
    return make_call(id, "filesystem.read_file", {{"path", rel}});
}

/**
 * @brief write_file of `rel` with `body`.
 * @param id Call id.
 * @param rel Root-relative path.
 * @param body New content.
 * @return The call.
 * @internal
 * @version 2.13.0
 */
entropic::ToolCall write_call(const std::string& id, const std::string& rel,
                              const std::string& body) {
    return make_call(id, "filesystem.write_file",
                     {{"path", rel}, {"content", body}});
}

/**
 * @brief entropic.todo add of `item`.
 * @param id Call id.
 * @param item Todo text.
 * @return The call.
 * @internal
 * @version 2.13.0
 */
entropic::ToolCall todo_add(const std::string& id, const std::string& item) {
    return make_call(id, "entropic.todo",
                     {{"action", "add"}, {"content", item}});
}

/**
 * @brief Whether a result is the read-before-write refusal.
 * @param r Result text.
 * @return true for a `read_before_write` error.
 * @internal
 * @version 2.13.0
 */
bool refused_unread(const std::string& r) {
    return r.find("read_before_write") != std::string::npos;
}

/**
 * @brief Whether a result is a successful write.
 * @param r Result text.
 * @return true when write_file reported success.
 * @internal
 * @version 2.13.0
 */
bool written(const std::string& r) {
    return r.find("File written successfully") != std::string::npos;
}

}  // namespace

SCENARIO("gh#158: two sessions reading and writing through ONE filesystem "
         "server share its read tracker safely",
         "[api][gh158][concurrency][v2.13.0]") {
    // read_file records into FileAccessTracker; write_file consults it.
    // Two unbound sessions share the default set's ONE FilesystemServer,
    // so both land on one tracker from two threads at once.
    TempDir root("tracker");
    constexpr int kRounds = 40;
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < kRounds; ++i) {
            root.put("t" + std::to_string(t) + "_" + std::to_string(i)
                     + ".txt", "v1\n");
        }
    }
    Handle h(root.path);
    REQUIRE(h.ok);
    REQUIRE(h.h->tool_executor != nullptr);

    GIVEN("two sessions, each reading then overwriting its own files") {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        // Results are collected, never asserted off the main thread: a
        // Catch2 assertion in a worker is not reliably reported.
        std::vector<std::vector<std::string>> seen(2);

        auto drive = [&](int t) {
            const std::string key = "s" + std::to_string(t);
            ready.fetch_add(1);
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < kRounds; ++i) {
                auto rel = "t" + std::to_string(t) + "_"
                         + std::to_string(i) + ".txt";
                auto id = key + "-" + std::to_string(i);
                auto res = dispatch(h.h, key, {
                    make_call(id + "r", "filesystem.read_file",
                              {{"path", rel}}),
                    make_call(id + "w", "filesystem.write_file",
                              {{"path", rel}, {"content", "v2 " + id}}),
                });
                auto& out = seen[static_cast<size_t>(t)];
                out.insert(out.end(), res.begin(), res.end());
            }
        };

        std::thread t0([&] { drive(0); });
        std::thread t1([&] { drive(1); });
        while (ready.load() < 2) { std::this_thread::yield(); }
        go.store(true);
        t0.join();
        t1.join();

        THEN("every write is let through by the read that preceded it") {
            for (size_t t = 0; t < 2; ++t) {
                REQUIRE(seen[t].size() == 2U * kRounds);
                for (size_t i = 1; i < seen[t].size(); i += 2) {
                    INFO("session s" << t << " call " << i << ": "
                         << seen[t][i]);
                    // A lost tracker entry turns into a read_before_write
                    // refusal of a file this session DID read.
                    CHECK(seen[t][i].find("File written successfully")
                          != std::string::npos);
                    CHECK(seen[t][i].find("read_before_write")
                          == std::string::npos);
                }
            }
        }
    }
}

SCENARIO("gh#158: a session's tool call never resolves inside another "
         "session's delegation sandbox",
         "[api][gh158][gh160][concurrency][v2.13.0]") {
    // gh#160 re-roots the DEFAULT server set for a sandboxed delegation's
    // whole child run. With concurrent_sessions on, a second UNBOUND
    // session dispatches on that same set meanwhile. The invariant: its
    // call resolves against ITS root — never the other session's sandbox.
    TempDir root("swap_root");
    root.put("notes.md", "MARKER-root\n");
    TempDir sandbox("swap_sandbox");
    sandbox.put("notes.md", "MARKER-sandbox\n");
    Handle h(root.path);
    REQUIRE(h.ok);
    REQUIRE(h.h->tool_executor != nullptr);

    GIVEN("session A inside a sandboxed delegation, session B dispatching "
          "while A's sandbox is in place") {
        std::atomic<bool> a_inside{false};
        std::atomic<bool> b_dispatching{false};
        std::atomic<bool> a_restoring{false};
        std::string a_saw;
        std::string b_saw;
        bool b_finished_after_restore = false;

        std::thread ta([&] {
            // Exactly what DelegationManager does around a child run.
            entropic::ScopedSandbox scope(entropic::swap_session_tool_dir,
                                          h.h, "s-a", sandbox.path,
                                          root.path);
            a_saw = joined(dispatch(h.h, "s-a", read_both("a", "notes.md")));
            a_inside.store(true);
            while (!b_dispatching.load()) { std::this_thread::yield(); }
            // B is now inside (or blocked in) its dispatch. Hold the
            // sandbox long enough that an unguarded B finishes first.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            a_restoring.store(true);
        });  // ~ScopedSandbox: restore, then release
        std::thread tb([&] {
            while (!a_inside.load()) { std::this_thread::yield(); }
            b_dispatching.store(true);
            b_saw = joined(dispatch(h.h, "s-b", read_both("b", "notes.md")));
            b_finished_after_restore = a_restoring.load();
        });
        ta.join();
        tb.join();
        INFO("A saw:\n" << a_saw << "\nB saw:\n" << b_saw);

        THEN("A's own child resolves inside its sandbox") {
            // The sandbox owner passes through its own lock — otherwise
            // the child would self-deadlock or escape its sandbox.
            CHECK(a_saw.find("MARKER-sandbox") != std::string::npos);
            CHECK(a_saw.find("MARKER-root") == std::string::npos);
        }
        THEN("B resolves against its own root, never A's sandbox") {
            // Both servers: filesystem (root_dir_) and bash (working_dir_).
            CHECK(b_saw.find("MARKER-root") != std::string::npos);
            CHECK(b_saw.find("MARKER-sandbox") == std::string::npos);
        }
        THEN("B's call waited for A's restore rather than racing it") {
            // Pins the CURRENT mechanism (the default set has one root, so
            // a sandbox excludes other sessions' dispatch on it). The
            // invariant is the THEN above; a per-session root design would
            // legitimately change this one.
            CHECK(b_finished_after_restore);
        }
    }
}

SCENARIO("gh#158: repeated sandbox swaps race nothing a concurrent "
         "session reads",
         "[api][gh158][gh160][concurrency][v2.13.0]") {
    // The race half: set_working_dir_all rewrites a std::filesystem::path
    // on three servers and RELOADS the ignore rules, while another run
    // thread resolves paths and matches ignore rules against them. The
    // tsan preset fails this binary on any report.
    TempDir root("churn_root");
    root.put("notes.md", "MARKER-root\n");
    root.put(".gitignore", "build/\n");
    TempDir sandbox("churn_sandbox");
    sandbox.put("notes.md", "MARKER-sandbox\n");
    sandbox.put(".gitignore", "out/\n");
    Handle h(root.path);
    REQUIRE(h.ok);

    GIVEN("A entering and leaving a sandbox while B keeps reading") {
        constexpr int kSwaps = 12;
        constexpr int kReads = 30;
        std::atomic<bool> go{false};
        std::vector<std::string> a_saw(kSwaps);
        std::vector<std::string> b_saw(kReads);

        std::thread ta([&] {
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < kSwaps; ++i) {
                entropic::ScopedSandbox scope(
                    entropic::swap_session_tool_dir, h.h, "s-a",
                    sandbox.path, root.path);
                a_saw[size_t(i)] = joined(dispatch(
                    h.h, "s-a", read_both("a" + std::to_string(i),
                                          "notes.md")));
            }
        });
        std::thread tb([&] {
            go.store(true);
            for (int i = 0; i < kReads; ++i) {
                b_saw[size_t(i)] = joined(dispatch(
                    h.h, "s-b", read_both("b" + std::to_string(i),
                                          "notes.md")));
            }
        });
        ta.join();
        tb.join();

        THEN("every A read is the sandbox and every B read is the root") {
            for (int i = 0; i < kSwaps; ++i) {
                INFO("A swap " << i << ": " << a_saw[size_t(i)]);
                CHECK(a_saw[size_t(i)].find("MARKER-root")
                      == std::string::npos);
            }
            for (int i = 0; i < kReads; ++i) {
                INFO("B read " << i << ": " << b_saw[size_t(i)]);
                CHECK(b_saw[size_t(i)].find("MARKER-sandbox")
                      == std::string::npos);
                CHECK(b_saw[size_t(i)].find("MARKER-root")
                      != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#158: two sessions writing todos through ONE entropic server "
         "share its list safely",
         "[api][gh158][concurrency][v2.13.0]") {
    // entropic.todo keeps its items in the TOOL object, and the default set
    // has one EntropicServer — so two unbound sessions append to one
    // std::vector from two run threads at once.
    TempDir root("todo");
    Handle h(root.path);
    REQUIRE(h.ok);

    GIVEN("two sessions each adding their own todos") {
        constexpr int kAdds = 30;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::string> last(2);

        auto drive = [&](int t) {
            const std::string key = "todo-s" + std::to_string(t);
            ready.fetch_add(1);
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < kAdds; ++i) {
                auto item = key + "-item-" + std::to_string(i);
                auto res = dispatch(h.h, key, {make_call(
                    item, "entropic.todo",
                    {{"action", "add"}, {"content", item}})});
                last[size_t(t)] = res.empty() ? "<no result>" : res.front();
            }
        };
        std::thread t0([&] { drive(0); });
        std::thread t1([&] { drive(1); });
        while (ready.load() < 2) { std::this_thread::yield(); }
        go.store(true);
        t0.join();
        t1.join();

        THEN("each session's final list still holds every item it added") {
            for (int t = 0; t < 2; ++t) {
                INFO("session " << t << " final result: " << last[size_t(t)]);
                for (int i = 0; i < kAdds; ++i) {
                    auto item = "todo-s" + std::to_string(t) + "-item-"
                              + std::to_string(i);
                    CHECK(last[size_t(t)].find(item) != std::string::npos);
                }
            }
        }
    }
}

// ── Per-SESSION state on a shared server (gh#158, fourth pass) ──────────
//
// The scenarios above prove the default set's tracker and todo list are
// SAFE to share. These prove they are not SHARED: a read recorded by one
// session is that session's alone, and so is its todo list. The key is the
// one the dispatch routed on (`LoopContext::session_key`), which a
// delegated child inherits from its parent.

SCENARIO("gh#158: read-before-write is judged per session — another "
         "session's read unlocks nothing",
         "[api][gh158][v2.13.0]") {
    TempDir root("rbw_session");
    root.put("shared.txt", "original\n");
    Handle h(root.path);
    REQUIRE(h.ok);
    REQUIRE(h.h->tool_executor != nullptr);

    GIVEN("session A has read shared.txt and session B has not") {
        auto a_read = one(h.h, "s-a", read_call("a-r", "shared.txt"));
        REQUIRE(a_read.find("original") != std::string::npos);

        WHEN("session B overwrites shared.txt") {
            auto b = one(h.h, "s-b",
                         write_call("b-w", "shared.txt", "from-b\n"));
            THEN("the write is refused as unread and the file is untouched") {
                INFO("B's write: " << b);
                CHECK(refused_unread(b));
                CHECK(slurp(root.path / "shared.txt") == "original\n");
            }
        }
        WHEN("session B edits shared.txt") {
            auto b = one(h.h, "s-b", make_call(
                "b-e", "filesystem.edit_file",
                {{"path", "shared.txt"}, {"old_string", "original"},
                 {"new_string", "edited-by-b"}}));
            THEN("the edit is refused as unread and the file is untouched") {
                INFO("B's edit: " << b);
                CHECK(refused_unread(b));
                CHECK(slurp(root.path / "shared.txt") == "original\n");
            }
        }
        WHEN("session A overwrites the file it read") {
            auto a = one(h.h, "s-a",
                         write_call("a-w", "shared.txt", "from-a\n"));
            THEN("its own read lets it through") {
                INFO("A's write: " << a);
                CHECK(written(a));
                CHECK(slurp(root.path / "shared.txt") == "from-a\n");
            }
        }
    }
}

namespace {

/**
 * @brief What the delegated child loop saw (driven by DelegationManager).
 * @internal
 * @version 2.13.0
 */
struct ChildDrive {
    entropic_handle_t h = nullptr;  ///< Handle whose executor it dispatches on
    std::string key = "<unset>";    ///< Session key the child ran under
    std::string read;               ///< Its read_file result
};

/**
 * @brief Child loop: read plan.md through the real executor, then finish.
 * @param ctx The context DelegationManager built for the child.
 * @param ud ChildDrive.
 * @internal
 * @version 2.13.0
 */
void child_reads_plan(entropic::LoopContext& ctx, void* ud) {
    auto* d = static_cast<ChildDrive*>(ud);
    d->key = ctx.session_key;
    auto msgs = d->h->tool_executor->process_tool_calls(
        ctx, {read_call("child-r", "plan.md")});
    d->read = msgs.empty() ? "<no result>" : msgs.front().content;
    entropic::Message done;
    done.role = "assistant";
    done.content = "read it";
    ctx.messages.push_back(std::move(done));
    ctx.state = entropic::AgentState::COMPLETE;
}

/**
 * @brief Tier resolution that accepts any tier (no prompt, no completion).
 * @param tier Tier name (unused).
 * @param ud Unused.
 * @return A valid, empty context info.
 * @internal
 * @version 2.13.0
 */
entropic::ChildContextInfo any_tier(const std::string& /*tier*/,
                                    void* /*ud*/) {
    entropic::ChildContextInfo info;
    info.valid = true;
    return info;
}

/**
 * @brief Tier existence check that accepts any tier.
 * @param tier Tier name (unused).
 * @param ud Unused.
 * @return true.
 * @internal
 * @version 2.13.0
 */
bool any_tier_exists(const std::string& /*tier*/, void* /*ud*/) {
    return true;
}

}  // namespace

SCENARIO("gh#158: a delegated child's read satisfies its parent's write — "
         "the child runs under the parent's session",
         "[api][gh158][v2.13.0]") {
    TempDir root("rbw_child");
    root.put("plan.md", "draft\n");
    Handle h(root.path);
    REQUIRE(h.ok);

    GIVEN("a parent session that delegated the read to a child") {
        ChildDrive drive;
        drive.h = h.h;
        entropic::TierResolutionInterface tiers;
        tiers.resolve_tier = any_tier;
        tiers.tier_exists = any_tier_exists;
        entropic::DelegationManager mgr(child_reads_plan, &drive, tiers);
        entropic::LoopContext parent;
        parent.session_key = "s-parent";
        auto res = mgr.execute_delegation(parent, "eng", "read plan.md");
        REQUIRE(res.success);
        REQUIRE(drive.read.find("draft") != std::string::npos);

        THEN("the child ran under the parent's key") {
            CHECK(drive.key == "s-parent");
        }
        THEN("the parent may overwrite the file its child read") {
            auto w = one(h.h, "s-parent",
                         write_call("p-w", "plan.md", "final\n"));
            INFO("parent's write: " << w);
            CHECK(written(w));
        }
        THEN("an unrelated session still may not") {
            auto w = one(h.h, "s-other",
                         write_call("o-w", "plan.md", "clobber\n"));
            INFO("other session's write: " << w);
            CHECK(refused_unread(w));
            CHECK(slurp(root.path / "plan.md") == "draft\n");
        }
    }
}

SCENARIO("gh#158: each session's todo list is its own",
         "[api][gh158][v2.13.0]") {
    TempDir root("todo_session");
    Handle h(root.path);
    REQUIRE(h.ok);

    GIVEN("session A has a todo on the shared entropic server") {
        auto a1 = one(h.h, "s-a", todo_add("a1", "alpha-task"));
        REQUIRE(a1.find("alpha-task") != std::string::npos);

        WHEN("session B adds its own") {
            auto b1 = one(h.h, "s-b", todo_add("b1", "beta-task"));
            THEN("B's list holds B's item and none of A's") {
                INFO("B's list: " << b1);
                CHECK(b1.find("beta-task") != std::string::npos);
                CHECK(b1.find("alpha-task") == std::string::npos);
                // Index 0 is B's own first item, not A's.
                CHECK(b1.find("0. [pending] beta-task") != std::string::npos);
            }
            THEN("A's next list holds none of B's") {
                auto a2 = one(h.h, "s-a", todo_add("a2", "alpha-two"));
                INFO("A's list: " << a2);
                CHECK(a2.find("alpha-task") != std::string::npos);
                CHECK(a2.find("beta-task") == std::string::npos);
            }
        }
    }
}

namespace {

/**
 * @brief How a session's conversation is ended in the release scenario.
 * @internal
 * @version 2.13.0
 */
enum class EndBy { drop, clear, restore };

/**
 * @brief End session `key`'s conversation through the named C API.
 * @param h Handle.
 * @param key Session key.
 * @param how Which API.
 * @return The API's return code.
 * @internal
 * @version 2.13.0
 */
entropic_error_t end_session(entropic_handle_t h, const char* key, EndBy how) {
    switch (how) {
    case EndBy::drop: return entropic_session_drop(h, key);
    case EndBy::clear: return entropic_session_context_clear(h, key);
    case EndBy::restore: return entropic_session_context_set(h, key, "[]");
    }
    return ENTROPIC_ERROR_INVALID_ARGUMENT;
}

/**
 * @brief Sessions holding state on one default-set server (white-box).
 * @param h Handle.
 * @param server "filesystem" or "entropic".
 * @return The server's session count, or SIZE_MAX when it is not a
 *         SessionStateOwner (which fails every equality below).
 * @internal
 * @version 2.13.0
 */
std::size_t sessions_on(entropic_handle_t h, const std::string& server) {
    auto* owner = dynamic_cast<entropic::SessionStateOwner*>(
        h->server_manager->get_server(server));
    return owner != nullptr ? owner->session_count() : SIZE_MAX;
}

}  // namespace

SCENARIO("gh#158: ending a session's conversation releases its read "
         "tracker and todo list — and only its own",
         "[api][gh158][v2.13.0]") {
    TempDir root("release");
    root.put("f.txt", "orig\n");
    Handle h(root.path);
    REQUIRE(h.ok);

    for (auto how : {EndBy::drop, EndBy::clear, EndBy::restore}) {
        const char* name = how == EndBy::drop ? "entropic_session_drop"
            : how == EndBy::clear ? "entropic_session_context_clear"
                                  : "entropic_session_context_set";
        DYNAMIC_SECTION("ended by " << name) {
            // A and B have each read f.txt and each hold a todo.
            REQUIRE(one(h.h, "s-a", read_call("a-r", "f.txt")).find("orig")
                    != std::string::npos);
            REQUIRE(one(h.h, "s-b", read_call("b-r", "f.txt")).find("orig")
                    != std::string::npos);
            one(h.h, "s-a", todo_add("a-t", "alpha-before"));
            one(h.h, "s-b", todo_add("b-t", "beta-kept"));
            REQUIRE(sessions_on(h.h, "filesystem") == 2U);
            REQUIRE(sessions_on(h.h, "entropic") == 2U);

            REQUIRE(end_session(h.h, "s-a", how) == ENTROPIC_OK);

            // Bounded growth: A's entries are GONE, not merely emptied.
            CHECK(sessions_on(h.h, "filesystem") == 1U);
            CHECK(sessions_on(h.h, "entropic") == 1U);

            // A's read died with its conversation: the model no longer
            // holds that content, so a write must re-read first.
            auto a_w = one(h.h, "s-a", write_call("a-w", "f.txt", "a\n"));
            INFO("A's write after " << name << ": " << a_w);
            CHECK(refused_unread(a_w));
            CHECK(slurp(root.path / "f.txt") == "orig\n");

            auto a_t = one(h.h, "s-a", todo_add("a-t2", "alpha-after"));
            INFO("A's list after " << name << ": " << a_t);
            CHECK(a_t.find("alpha-before") == std::string::npos);
            CHECK(a_t.find("alpha-after") != std::string::npos);

            // B's state is untouched.
            auto b_t = one(h.h, "s-b", todo_add("b-t2", "beta-two"));
            INFO("B's list: " << b_t);
            CHECK(b_t.find("beta-kept") != std::string::npos);
            auto b_w = one(h.h, "s-b", write_call("b-w", "f.txt", "b\n"));
            INFO("B's write: " << b_w);
            CHECK(written(b_w));
        }
    }
}

SCENARIO("gh#158: clearing the default conversation releases the default "
         "session's read tracker",
         "[api][gh158][v2.13.0]") {
    TempDir root("release_default");
    root.put("f.txt", "orig\n");
    Handle h(root.path);
    REQUIRE(h.ok);

    GIVEN("the default session has read f.txt") {
        REQUIRE(one(h.h, "", read_call("d-r", "f.txt")).find("orig")
                != std::string::npos);
        WHEN("entropic_context_clear runs") {
            REQUIRE(entropic_context_clear(h.h) == ENTROPIC_OK);
            THEN("a write must re-read first") {
                auto w = one(h.h, "", write_call("d-w", "f.txt", "x\n"));
                INFO("default write: " << w);
                CHECK(refused_unread(w));
                CHECK(slurp(root.path / "f.txt") == "orig\n");
            }
        }
    }
}
