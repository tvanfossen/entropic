// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file path_approval_test.cpp
 * @brief v2.13.0: outside-root file access asks the host's approver by
 *        default, with explicit allow and deny lists.
 *
 * The finding that motivated it: `data/default_config.yaml` shipped
 * `mcp.filesystem.allow_outside_root: true`, so every consumer running on
 * the bundled defaults handed the model unconfined READ and WRITE of the
 * whole filesystem through `filesystem.read_file` / `write_file` /
 * `edit_file` / `list_directory`. Nobody chose that; it was the default.
 *
 * `allow_outside_root` is now tri-state — `true` | `false` | `optional` —
 * and defaults to `optional`: an escaping path goes to the host's path
 * approver with the resolved path and read/write, and is refused with a
 * typed, explicit message when no approver is registered. Two path lists
 * refine it: `outside_root_allow` (pre-approved, no prompt) and
 * `outside_root_deny` (always refused). Deny beats allow beats the
 * tri-state; matching is by canonical SUBTREE, never string prefix.
 *
 * White-box (`engine_handle.h`) for the dispatch, like workspace_test.cpp:
 * the property is where a path lands, which needs no model.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include <entropic/config/bundled_models.h>
#include <entropic/config/loader.h>
#include <entropic/mcp/servers/filesystem.h>
#include "engine_handle.h"  // white-box: server_manager, tool_executor

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief Marker written into every file outside the root.
/// @internal
/// @version 2.13.0
constexpr const char* kSecret = "OUTSIDE-SECRET-7f3a";

/**
 * @brief A project root and a SIBLING tree outside it, both temporary.
 *
 * `outside/` holds `secret.txt`, `data/f.txt`, `database/f.txt` and
 * `private/key.txt` — enough shape for every precedence rule, including
 * the `/data` vs `/database` subtree boundary.
 *
 * @internal
 * @version 2.13.0
 */
struct Tree {
    fs::path root;     ///< Project root the servers are confined to
    fs::path outside;  ///< Sibling tree — outside the root

    /**
     * @brief Create both trees under the temp dir.
     * @param tag Distinguishes concurrent test processes' trees.
     * @internal
     * @version 2.13.0
     */
    explicit Tree(const std::string& tag) {
        static std::atomic<int> n{0};
        auto base = fs::weakly_canonical(fs::temp_directory_path()) /
                    ("entropic_pa_" + tag + "_" + std::to_string(::getpid())
                     + "_" + std::to_string(n.fetch_add(1)));
        fs::remove_all(base);
        root = base / "project";
        outside = base / "elsewhere";
        fs::create_directories(root);
        for (const char* sub : {"data", "database", "private"}) {
            fs::create_directories(outside / sub);
        }
        std::ofstream(root / "inside.txt") << "INSIDE\n";
        std::ofstream(outside / "secret.txt") << kSecret << "\n";
        std::ofstream(outside / "data" / "f.txt") << kSecret << "-data\n";
        std::ofstream(outside / "database" / "f.txt") << kSecret << "-db\n";
        std::ofstream(outside / "private" / "key.txt") << kSecret << "-key\n";
    }

    /** @brief Remove both trees. @internal @version 2.13.0 */
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
};

/**
 * @brief Configure JSON rooted at `t.root` with a `mcp.filesystem` body.
 * @param t Tree whose root becomes `mcp.working_dir`.
 * @param filesystem Body of the `filesystem` object ("" = key absent).
 * @param auto_approve `permissions.auto_approve`.
 * @return JSON accepted by entropic_configure.
 * @internal
 * @version 2.13.0
 */
std::string config_json(const Tree& t, const std::string& filesystem,
                        bool auto_approve = false) {
    return std::string(R"({"log_level":"WARN","permissions":{"auto_approve":)")
         + (auto_approve ? "true" : "false")
         + R"(},"mcp":{"working_dir":")" + t.root.string()
         + R"(","filesystem":{)" + filesystem + "}}}";
}

/**
 * @brief RAII handle configured from JSON.
 * @internal
 * @version 2.13.0
 */
struct Handle {
    entropic_handle_t h = nullptr;  ///< Handle under test
    bool ok = false;                ///< Whether configure succeeded

    /** @brief Create + configure. @internal @version 2.13.0 */
    explicit Handle(const std::string& json) {
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
 * @brief Tool-call arguments naming one path.
 * @param p Path.
 * @return `{"path":"<p>"}`.
 * @internal
 * @version 2.13.0
 */
std::string path_args(const fs::path& p) {
    return R"({"path":")" + p.string() + R"("})";
}

/**
 * @brief `filesystem.read_file` through the handle's default server set.
 * @param h Configured handle.
 * @param p Path to read.
 * @return ServerResponse envelope JSON.
 * @internal
 * @version 2.13.0
 */
std::string read_via(entropic_handle_t h, const fs::path& p) {
    return h->server_manager->execute("filesystem.read_file", path_args(p));
}

/**
 * @brief Whether `hay` contains `needle`.
 * @internal
 * @version 2.13.0
 */
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── The finding itself ───────────────────────────────────

SCENARIO("outside-root: the SHIPPED default config no longer hands the model "
         "the whole filesystem", "[api][filesystem][outside_root][v2.13.0]") {
    // Parsed from the real data/default_config.yaml, because the property
    // is about the file consumers run with — a test that builds its own
    // FilesystemConfig{} would pass while the bundled file still said true.
    entropic::config::BundledModels registry;
    REQUIRE(registry.load(fs::path(ENTROPIC_REPO_DATA_DIR)
                          / "bundled_models.yaml").empty());
    entropic::ParsedConfig cfg;
    REQUIRE(entropic::config::parse_config_file(
                fs::path(ENTROPIC_REPO_DATA_DIR) / "default_config.yaml",
                registry, cfg).empty());

    Tree t("bundled");
    entropic::FilesystemServer server(t.root, cfg.mcp.filesystem,
                                      ENTROPIC_REPO_DATA_DIR);

    WHEN("the model reads a file outside the root and no approver exists") {
        auto out = server.execute("read_file", path_args(t.outside / "secret.txt"));
        INFO(out);
        THEN("nothing outside the root is read") {
            CHECK_FALSE(has(out, kSecret));
        }
        THEN("the refusal is typed and says approval was required") {
            CHECK(has(out, "outside_root_approval_required"));
        }
    }
}

// ── optional: the new default ────────────────────────────

SCENARIO("outside-root: an OMITTED allow_outside_root is `optional` and "
         "fails loud without an approver",
         "[api][filesystem][outside_root][v2.13.0]") {
    // Repo rule: every optional config key gets a test where it is ABSENT.
    Tree t("omitted");
    Handle h(config_json(t, ""));
    REQUIRE(h.ok);

    auto target = t.outside / "secret.txt";
    auto out = read_via(h.h, target);
    INFO(out);
    THEN("the file is not read") {
        CHECK_FALSE(has(out, kSecret));
    }
    THEN("the refusal names its type, the path, and how to configure it") {
        CHECK(has(out, "outside_root_approval_required"));
        CHECK(has(out, target.string()));
        CHECK(has(out, "outside_root_allow"));
        CHECK(has(out, "allow_outside_root: true"));
    }
    THEN("a path INSIDE the root is unaffected") {
        auto in = read_via(h.h, t.root / "inside.txt");
        INFO(in);
        CHECK(has(in, "INSIDE"));
    }
}

SCENARIO("outside-root: an explicit `optional` behaves as the default",
         "[api][filesystem][outside_root][v2.13.0]") {
    Tree t("explicit_optional");
    Handle h(config_json(t, R"("allow_outside_root":"optional")"));
    REQUIRE(h.ok);

    auto out = read_via(h.h, t.outside / "secret.txt");
    INFO(out);
    CHECK_FALSE(has(out, kSecret));
    CHECK(has(out, "outside_root_approval_required"));
}

// ── Precedence: deny > allow > tri-state ─────────────────

SCENARIO("outside-root: outside_root_deny beats allow_outside_root: true and "
         "beats outside_root_allow",
         "[api][filesystem][outside_root][v2.13.0]") {
    Tree t("deny");

    GIVEN("allow_outside_root: true with the whole outside tree denied") {
        Handle h(config_json(t, R"("allow_outside_root":true,)"
            R"("outside_root_deny":[")" + t.outside.string() + R"("])"));
        REQUIRE(h.ok);
        auto out = read_via(h.h, t.outside / "secret.txt");
        INFO(out);
        THEN("true does not reach a denied path") {
            CHECK_FALSE(has(out, kSecret));
            CHECK(has(out, "outside_root_denied"));
        }
    }

    GIVEN("the outside tree allowed and one subtree of it denied") {
        Handle h(config_json(t,
            R"("outside_root_allow":[")" + t.outside.string() + R"("],)"
            R"("outside_root_deny":[")" + (t.outside / "private").string()
            + R"("])"));
        REQUIRE(h.ok);
        THEN("the denied subtree is refused") {
            auto out = read_via(h.h, t.outside / "private" / "key.txt");
            INFO(out);
            CHECK_FALSE(has(out, kSecret));
            CHECK(has(out, "outside_root_denied"));
        }
        THEN("the rest of the allowed tree is read without a prompt") {
            auto out = read_via(h.h, t.outside / "secret.txt");
            INFO(out);
            CHECK(has(out, kSecret));
        }
    }
}

SCENARIO("outside-root: an allowlisted subtree is readable even under "
         "`false`, matched by subtree and never by string prefix",
         "[api][filesystem][outside_root][v2.13.0]") {
    Tree t("subtree");
    Handle h(config_json(t, R"("allow_outside_root":false,)"
        R"("outside_root_allow":[")" + (t.outside / "data").string()
        + R"("])"));
    REQUIRE(h.ok);

    THEN("a file inside the allowed subtree is read") {
        auto out = read_via(h.h, t.outside / "data" / "f.txt");
        INFO(out);
        CHECK(has(out, std::string(kSecret) + "-data"));
    }
    THEN("`/data` does not admit its string-prefix sibling `/database`") {
        auto out = read_via(h.h, t.outside / "database" / "f.txt");
        INFO(out);
        CHECK_FALSE(has(out, kSecret));
        CHECK(has(out, "Path escapes project root"));
    }
}

// ── Legacy spellings keep their meaning ──────────────────

SCENARIO("outside-root: legacy `true` and `false` mean what they always did",
         "[api][filesystem][outside_root][v2.13.0]") {
    Tree t("legacy");

    GIVEN("allow_outside_root: true") {
        Handle h(config_json(t, R"("allow_outside_root":true)"));
        REQUIRE(h.ok);
        auto out = read_via(h.h, t.outside / "secret.txt");
        INFO(out);
        THEN("an outside read is served, no prompt") {
            CHECK(has(out, kSecret));
        }
    }

    GIVEN("allow_outside_root: false") {
        Handle h(config_json(t, R"("allow_outside_root":false)"));
        REQUIRE(h.ok);
        auto out = read_via(h.h, t.outside / "secret.txt");
        INFO(out);
        THEN("an outside read is refused with the pre-2.13 message") {
            CHECK_FALSE(has(out, kSecret));
            CHECK(has(out, "Path escapes project root: "
                           + (t.outside / "secret.txt").string()));
            CHECK_FALSE(has(out, "outside_root_approval_required"));
        }
    }
}

// ── auto_approve is not a filesystem boundary ────────────

SCENARIO("outside-root: permissions.auto_approve does NOT approve a root "
         "escape", "[api][filesystem][outside_root][v2.13.0]") {
    // auto_approve skips the per-TOOL permission prompt. Widening the
    // filesystem boundary is a different decision, made with
    // outside_root_allow or allow_outside_root: true — so an auto_approve
    // host with no path approver is refused exactly like any other.
    Tree t("auto_approve");
    Handle h(config_json(t, "", /*auto_approve=*/true));
    REQUIRE(h.ok);
    REQUIRE(h.h->tool_executor != nullptr);

    entropic::ToolCall call;
    call.id = "c1";
    call.name = "filesystem.read_file";
    call.arguments["path"] = (t.outside / "secret.txt").string();
    call.arguments_json = path_args(t.outside / "secret.txt");

    entropic::LoopContext ctx;
    auto msgs = h.h->tool_executor->process_tool_calls(ctx, {call});
    REQUIRE(msgs.size() == 1);
    INFO(msgs.front().content);
    THEN("the call passed the tool gate but the path was still refused") {
        CHECK_FALSE(has(msgs.front().content, "denied"));
        CHECK_FALSE(has(msgs.front().content, kSecret));
        CHECK(has(msgs.front().content, "outside_root_approval_required"));
    }
}

// ── Workspaces stay HARD-confined ────────────────────────

SCENARIO("outside-root: a workspace refuses even an allowlisted outside "
         "path", "[api][filesystem][outside_root][workspace][v2.13.0]") {
    // gh#166/ef6516d: inside a workspace, "outside my root" means "inside
    // ANOTHER repository". Neither the host's allow list nor its approver
    // may turn a sibling workspace's files into "just approve it".
    Tree t("workspace");
    Handle h(config_json(t, R"("allow_outside_root":true,)"
        R"("outside_root_allow":[")" + t.outside.string() + R"("])"));
    REQUIRE(h.ok);

    auto ws_root = t.root / "ws";
    fs::create_directories(ws_root);
    REQUIRE(entropic_workspace_create(h.h, "ws", ws_root.c_str())
            == ENTROPIC_OK);
    REQUIRE(entropic_session_bind_workspace(h.h, "s", "ws") == ENTROPIC_OK);
    auto* ws = entropic::workspace_servers(h.h, "s");
    REQUIRE(ws != nullptr);
    REQUIRE(ws != h.h->server_manager.get());

    auto target = t.outside / "secret.txt";
    THEN("the workspace's own server refuses it") {
        auto out = ws->execute("filesystem.read_file", path_args(target));
        INFO(out);
        CHECK_FALSE(has(out, kSecret));
        CHECK(has(out, "Path escapes project root"));
    }
    THEN("the handle's DEFAULT set still honours the host's allow list") {
        auto out = read_via(h.h, target);
        INFO(out);
        CHECK(has(out, kSecret));
    }
}

// ── The host's approver, through the public C API ────────

namespace {

/**
 * @brief Consumer-side approver: records requests, answers by path.
 *
 * Strings are copied — the request is only valid for the callback.
 * @internal
 * @version 2.13.0
 */
struct HostApprover {
    std::string approve_path;          ///< The one path to ACCEPT
    std::vector<std::string> paths;    ///< Every path asked about
    std::vector<std::string> roots;    ///< Every root reported
    std::vector<std::string> tools;    ///< Every tool reported
    std::vector<ent_path_access_t> access; ///< Every access kind
    std::vector<std::string> sessions; ///< Every session key

    /** @brief C trampoline. @internal @version 2.13.0 */
    static ent_decision_t call(const ent_path_approval_request_t* req,
                               void* ud) {
        auto* self = static_cast<HostApprover*>(ud);
        self->paths.emplace_back(req->path);
        self->roots.emplace_back(req->root);
        self->tools.emplace_back(req->tool);
        self->access.push_back(req->access);
        self->sessions.emplace_back(req->session_key);
        return self->approve_path == req->path ? ENT_DECISION_ACCEPT
                                               : ENT_DECISION_REJECT;
    }
};

}  // namespace

SCENARIO("outside-root: entropic_set_path_approval_callback rejects a NULL "
         "handle", "[api][filesystem][outside_root][v2.13.0]") {
    CHECK(entropic_set_path_approval_callback(nullptr, nullptr, nullptr)
          == ENTROPIC_ERROR_INVALID_HANDLE);
}

SCENARIO("outside-root: the host approver decides each outside access",
         "[api][filesystem][outside_root][v2.13.0]") {
    Tree t("host_approver");
    Handle h(config_json(t, ""));  // omitted key → optional
    REQUIRE(h.ok);
    HostApprover approver;
    approver.approve_path = (t.outside / "secret.txt").string();
    REQUIRE(entropic_set_path_approval_callback(
                h.h, &HostApprover::call, &approver) == ENTROPIC_OK);

    WHEN("the approver accepts a read") {
        auto out = read_via(h.h, t.outside / "secret.txt");
        INFO(out);
        THEN("the file is served") { CHECK(has(out, kSecret)); }
        THEN("the request carried the canonical path, root, tool and READ") {
            REQUIRE(approver.paths.size() == 1);
            CHECK(approver.paths[0] == (t.outside / "secret.txt").string());
            CHECK(approver.roots[0] == t.root.string());
            CHECK(approver.tools[0] == "filesystem.read_file");
            CHECK(approver.access[0] == ENT_PATH_ACCESS_READ);
            CHECK(approver.sessions[0].empty());  // no keyed run
        }
    }

    WHEN("the approver rejects a read") {
        auto out = read_via(h.h, t.outside / "private" / "key.txt");
        INFO(out);
        THEN("it is refused with the rejection type, nothing leaked") {
            CHECK_FALSE(has(out, kSecret));
            CHECK(has(out, "outside_root_rejected"));
        }
    }

    WHEN("the model writes outside the root and the approver rejects") {
        auto planted = t.outside / "planted.txt";
        auto out = h.h->server_manager->execute(
            "filesystem.write_file",
            R"({"path":")" + planted.string() + R"(","content":"x"})");
        INFO(out);
        THEN("the approver was asked about a WRITE and nothing was written") {
            REQUIRE(approver.access.size() == 1);
            CHECK(approver.access[0] == ENT_PATH_ACCESS_WRITE);
            CHECK(approver.tools[0] == "filesystem.write_file");
            CHECK(has(out, "outside_root_rejected"));
            CHECK_FALSE(fs::exists(planted));
        }
    }

    WHEN("the callback is cleared") {
        REQUIRE(entropic_set_path_approval_callback(h.h, nullptr, nullptr)
                == ENTROPIC_OK);
        auto out = read_via(h.h, t.outside / "secret.txt");
        INFO(out);
        THEN("escapes go back to the typed no-approver refusal") {
            CHECK_FALSE(has(out, kSecret));
            CHECK(has(out, "outside_root_approval_required"));
            CHECK(approver.paths.empty());
        }
    }
}

SCENARIO("outside-root: an approver registered BEFORE configure is honoured",
         "[api][filesystem][outside_root][v2.13.0]") {
    // REQ-API-010: every consumer slot survives configure.
    Tree t("preconfigure");
    HostApprover approver;
    approver.approve_path = (t.outside / "secret.txt").string();

    entropic_handle_t h = nullptr;
    REQUIRE(entropic_create(&h) == ENTROPIC_OK);
    REQUIRE(entropic_set_path_approval_callback(
                h, &HostApprover::call, &approver) == ENTROPIC_OK);
    REQUIRE(entropic_configure(h, config_json(t, "").c_str())
            == ENTROPIC_OK);

    auto out = read_via(h, t.outside / "secret.txt");
    INFO(out);
    CHECK(has(out, kSecret));
    CHECK(approver.paths.size() == 1);
    entropic_destroy(h);
}

SCENARIO("outside-root: auto_approve does not pre-empt the path approver",
         "[api][filesystem][outside_root][v2.13.0]") {
    // The two gates are independent: auto_approve passes the TOOL gate,
    // and the path approver still owns the filesystem boundary.
    Tree t("auto_plus_approver");
    Handle h(config_json(t, "", /*auto_approve=*/true));
    REQUIRE(h.ok);
    HostApprover approver;  // approves nothing
    REQUIRE(entropic_set_path_approval_callback(
                h.h, &HostApprover::call, &approver) == ENTROPIC_OK);

    entropic::ToolCall call;
    call.id = "c1";
    call.name = "filesystem.read_file";
    call.arguments["path"] = (t.outside / "secret.txt").string();
    call.arguments_json = path_args(t.outside / "secret.txt");
    entropic::LoopContext ctx;
    auto msgs = h.h->tool_executor->process_tool_calls(ctx, {call});
    REQUIRE(msgs.size() == 1);
    INFO(msgs.front().content);
    CHECK(approver.paths.size() == 1);
    CHECK(has(msgs.front().content, "outside_root_rejected"));
    CHECK_FALSE(has(msgs.front().content, kSecret));
}

SCENARIO("outside-root: a workspace never consults the host's approver",
         "[api][filesystem][outside_root][workspace][v2.13.0]") {
    Tree t("workspace_approver");
    Handle h(config_json(t, ""));  // optional
    REQUIRE(h.ok);
    HostApprover approver;
    approver.approve_path = (t.outside / "secret.txt").string();
    REQUIRE(entropic_set_path_approval_callback(
                h.h, &HostApprover::call, &approver) == ENTROPIC_OK);

    auto ws_root = t.root / "ws";
    fs::create_directories(ws_root);
    REQUIRE(entropic_workspace_create(h.h, "ws", ws_root.c_str())
            == ENTROPIC_OK);
    REQUIRE(entropic_session_bind_workspace(h.h, "s", "ws") == ENTROPIC_OK);
    auto* ws = entropic::workspace_servers(h.h, "s");
    REQUIRE(ws != nullptr);

    auto out = ws->execute("filesystem.read_file",
                           path_args(t.outside / "secret.txt"));
    INFO(out);
    THEN("the workspace refuses outright and the approver is never asked") {
        CHECK_FALSE(has(out, kSecret));
        CHECK(has(out, "Path escapes project root"));
        CHECK(approver.paths.empty());
    }
}

// ── The slot is swapped while run threads consult it ─────

namespace {

/** @brief Always-accept approver. @internal @version 2.13.0 */
ent_decision_t accept_all(const ent_path_approval_request_t* /*req*/,
                          void* ud) {
    static_cast<std::atomic<int>*>(ud)->fetch_add(1);
    return ENT_DECISION_ACCEPT;
}

}  // namespace

SCENARIO("outside-root: registering and clearing the approver while tool "
         "calls consult it is race-free",
         "[api][filesystem][outside_root][concurrency][v2.13.0]") {
    // With concurrent_sessions on, the approver fires on run threads while
    // the host may (re)register it from any thread. The slot is a PAIR
    // (fn, user_data); a torn read would call one host's callback with
    // another's data. Run under the tsan preset to make a race fatal.
    //
    // list_directory, not read_file: read_file also writes the server's
    // FileAccessTracker, whose own thread-safety is not what this checks.
    Tree t("concurrent_slot");
    Handle h(config_json(t, ""));
    REQUIRE(h.ok);

    constexpr int kCalls = 200;
    std::atomic<int> accepted{0};
    std::atomic<bool> stop{false};
    std::atomic<int> unexpected{0};

    auto reader = [&] {
        for (int i = 0; i < kCalls; ++i) {
            auto out = h.h->server_manager->execute(
                "filesystem.list_directory", path_args(t.outside));
            // Exactly two legal outcomes: served (the listing names
            // secret.txt) or the typed no-approver refusal.
            bool served = has(out, "secret.txt")
                && !has(out, "outside_root_");
            bool refused = has(out, "outside_root_approval_required");
            if (!(served || refused)) { unexpected.fetch_add(1); }
        }
    };
    std::thread writer([&] {
        while (!stop.load()) {
            entropic_set_path_approval_callback(h.h, &accept_all, &accepted);
            entropic_set_path_approval_callback(h.h, nullptr, nullptr);
        }
    });
    std::thread r1(reader);
    std::thread r2(reader);
    r1.join();
    r2.join();
    stop.store(true);
    writer.join();

    THEN("every call saw either the approver or none — nothing else") {
        CHECK(unexpected.load() == 0);
    }
}
