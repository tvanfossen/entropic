// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_root_lock_test.cpp
 * @brief gh#158 (v2.13.0): the root lock's three properties, one each.
 *
 * The behavioural claim — another session's dispatch never lands in a
 * sandbox — is proven end to end in tests/unit/api/
 * builtin_server_sharing_test.cpp. These pin the primitive underneath it,
 * because each property is load-bearing in a different place: owner
 * pass-through (a sandboxed child's own calls), shared-waits-for-exclusive
 * (the invariant), exclusive-waits-for-shared (no root moves under an
 * in-flight call).
 *
 * @version 2.13.0
 */

#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_root_lock.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using entropic::ToolRootLock;
using entropic::ToolRootShared;

namespace {

/// @brief How long a "blocked" thread is given to (wrongly) get through.
/// @internal
/// @version 2.13.0
constexpr auto kHold = std::chrono::milliseconds(150);

/**
 * @brief In-process server that records the directory it was moved to.
 * @internal
 * @version 2.13.0
 */
class RecordingServer : public entropic::MCPServerBase {
public:
    /** @brief Construct. @internal @version 2.13.0 */
    RecordingServer() : MCPServerBase("recording") {}

    /**
     * @brief Record the requested directory.
     * @param path New working directory.
     * @return Always true.
     * @internal
     * @version 2.13.0
     */
    bool set_working_dir(const std::string& path) override {
        last_dir = path;
        return true;
    }

    std::string last_dir;  ///< Last directory the manager asked for
};

}  // namespace

TEST_CASE("gh#158: the root lock's owner re-enters and passes through "
          "its own shared side",
          "[mcp][tool_root_lock][gh158][v2.13.0]") {
    ToolRootLock lock;
    lock.lock();
    CHECK(lock.owned_by_this_thread());
    lock.lock();  // a nested delegation re-enters on the same thread
    {
        // The sandboxed child's own dispatch: must not self-deadlock.
        ToolRootShared own(lock, "owner-dispatch");
        CHECK(lock.owned_by_this_thread());
    }
    lock.unlock();
    CHECK(lock.owned_by_this_thread());  // still one level held
    lock.unlock();
    CHECK_FALSE(lock.owned_by_this_thread());

    // Fully released: another thread takes the shared side at once.
    std::atomic<bool> got{false};
    std::thread other([&] {
        ToolRootShared hold(lock, "after-release");
        got.store(true);
    });
    other.join();
    CHECK(got.load());
}

TEST_CASE("gh#158: another thread's dispatch waits while the root lock is "
          "held exclusively",
          "[mcp][tool_root_lock][gh158][v2.13.0]") {
    ToolRootLock lock;
    std::atomic<bool> released{false};
    std::atomic<bool> saw_released{false};
    lock.lock();
    std::thread reader([&] {
        ToolRootShared hold(lock, "waiting-dispatch");
        saw_released.store(released.load());
    });
    std::this_thread::sleep_for(kHold);
    released.store(true);
    lock.unlock();
    reader.join();
    CHECK(saw_released.load());
}

TEST_CASE("gh#158: re-rooting waits for an in-flight dispatch",
          "[mcp][tool_root_lock][gh158][v2.13.0]") {
    ToolRootLock lock;
    std::atomic<bool> holding{false};
    std::atomic<bool> done{false};
    std::atomic<bool> saw_done{false};
    std::thread reader([&] {
        ToolRootShared hold(lock, "in-flight-dispatch");
        holding.store(true);
        std::this_thread::sleep_for(kHold);
        done.store(true);
    });
    while (!holding.load()) { std::this_thread::yield(); }
    {
        std::lock_guard<ToolRootLock> swap(lock);
        saw_done.store(done.load());
    }
    reader.join();
    CHECK(saw_done.load());
}

TEST_CASE("gh#158: leave_working_dir refuses a thread that never entered",
          "[mcp][server_manager][gh158][gh160][v2.13.0]") {
    // An unbalanced restore from the wrong thread would release a lock it
    // never took (undefined behaviour on the shared_mutex) and move the
    // servers out of a sandbox another thread's child is still using.
    entropic::PermissionsConfig perms;
    entropic::ServerManager mgr(perms, "/tmp");
    auto server = std::make_unique<RecordingServer>();
    auto* rec = server.get();
    mgr.register_server(std::move(server));

    CHECK(mgr.leave_working_dir("/tmp") == 0U);  // never entered: refused
    CHECK(rec->last_dir.empty());

    CHECK(mgr.enter_working_dir("/tmp/sbx") == 1U);
    CHECK(rec->last_dir == "/tmp/sbx");
    std::atomic<size_t> intruder_moved{99};
    std::thread intruder([&] {
        intruder_moved.store(mgr.leave_working_dir("/tmp"));
    });
    intruder.join();
    CHECK(intruder_moved.load() == 0U);
    CHECK(rec->last_dir == "/tmp/sbx");          // the sandbox stayed put
    CHECK(mgr.leave_working_dir("/tmp") == 1U);  // the owner's own restore
    CHECK(rec->last_dir == "/tmp");

    // Balanced: the lock is free again for another thread.
    std::atomic<bool> entered{false};
    std::thread next([&] {
        entered.store(mgr.enter_working_dir("/tmp/next") == 1U);
        mgr.leave_working_dir("/tmp");
    });
    next.join();
    CHECK(entered.load());
}
