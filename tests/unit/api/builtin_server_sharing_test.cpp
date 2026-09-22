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
#include <entropic/core/sandbox.h>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: tool_executor, swap_session_tool_dir

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
