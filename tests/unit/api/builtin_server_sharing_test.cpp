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
 * the path a run takes. The race half of each claim is proven by the
 * `tsan` preset (a report fails the test binary); the behavioural half is
 * asserted here, so the CPU lane catches a wrong answer too.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: tool_executor, server_manager

#include <nlohmann/json.hpp>

#include <atomic>
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
