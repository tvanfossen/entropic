---
id: P2-20260429-001
title: "Async Bridge I/O Architecture: per-subscriber queues + executor pool"
priority: 2
status: BACKLOG
created: 2026-04-29
updated: 2026-04-29
version_target: TBD (v2.2 candidate)
author: "@architect"
related: [P2-20260421-001]
---

## Summary

Replace the current bridge I/O model — blocking `write()` per subscriber +
`std::thread(...).detach()` per async task — with a bounded async pipeline:
per-subscriber outbound queue with a dedicated writer thread, plus a
fixed-size task executor pool fed by a FIFO request queue. Adds explicit
admission control and a documented queue-full policy. Closes the broader
class of async concurrency hazards that v2.1.2's issue #4 fix only
patches at the symptom level.

## Motivation

Issue #4 (v2.1.1, fixed in v2.1.2) exposed a deadlock where a slow MCP
consumer could wedge the bridge's async-task thread by failing to drain
notifications. v2.1.2 ships **A+B+C+D**:
- A: spec-compliant `notifications/progress` method
- B: drop inline result body (consumers fetch via `ask_status`)
- C: non-blocking `send()` with `MSG_DONTWAIT`; drop slow subscribers
- D: thread per accepted client (no single-client wedge)

That stops the bleeding. It does not give consumers a real concurrency
contract. Specifically:

- **Thread storm under load.** `run_async_ask` does
  `std::thread(...).detach()` per submitted task. Submit 100 async asks
  → 100 detached threads, all waiting on the engine's `api_mutex`. No
  upper bound. No admission control. No FIFO ordering guarantee.
- **No backpressure semantics.** v2.1.2's drop-slow-subscriber on
  `EAGAIN` is correct under the single-message broadcast model, but it
  silently *loses* notifications. A real queue would give consumers a
  bounded buffer with a documented overflow policy.
- **No bounded resource use.** Memory and thread count grow linearly
  with offered load. A small DoS surface.

A consumer asking "can I submit 20 concurrent requests and have the
engine work through them as compute allows?" gets *no* meaningful
guarantee from the v2.1.2 design — they get 20 detached threads that
all serialize at `api_mutex` and 20 broadcast attempts that drop on
backpressure.

## Current State (post-v2.1.2)

```
                    Bridge subprocess
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  accept_loop ──> accept() ──> std::thread per fd  (D)    │
│                                  │                       │
│                                  ▼                       │
│                          serve_client(fd)                │
│                                  │                       │
│                                  ▼                       │
│                          dispatch_ask(async=true)        │
│                                  │                       │
│                                  ▼                       │
│                          run_async_ask                   │
│                                  │                       │
│                                  ▼                       │
│                       std::thread(...).detach()  ◄── unbounded
│                                  │                       │
│                                  ▼                       │
│                          entropic_run [api_mutex]        │
│                                  │                       │
│                                  ▼                       │
│                       broadcast_notification             │
│                                  │                       │
│                                  ▼                       │
│                       send(fd, ..., MSG_DONTWAIT)  (C)   │
│                       drop on EAGAIN/EBADF/EPIPE         │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

`api_mutex` (per-handle, inside `entropic_run`) serializes engine
compute. Until P2-20260421-001 lands, **N=1 worker is the actual
parallel-compute limit** regardless of bridge concurrency.

## Design

### Per-subscriber outbound queue + writer thread

Each subscriber's fd becomes a `(fd, queue, writer_thread)` triple:

```cpp
struct Subscriber {
    int fd;
    std::deque<std::string> outbound;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> alive{true};
    std::thread writer;
};
std::unordered_map<int, std::shared_ptr<Subscriber>> subscribers_;
```

`broadcast_notification` enqueues onto each subscriber's queue under
its mutex and notifies the cv. The writer thread blocks on the cv,
drains the queue with blocking `send()` (no MSG_DONTWAIT — the writer
is the only place that blocks, and it blocks only itself, not the
async-task thread that emitted the notification).

Bounded queue: configurable depth (default 256). Overflow policy
options below.

### Task executor pool with FIFO queue

Replace `run_async_ask`'s `std::thread(...).detach()` with submission
to a bounded FIFO queue, drained by N worker threads:

```cpp
struct AsyncTaskRequest {
    std::string task_id;
    std::string prompt;
    std::string call_id;
};
BoundedQueue<AsyncTaskRequest> task_queue_{config.max_pending};
std::vector<std::thread> task_workers_;
```

Worker threads block on the queue, pop a request, call `entropic_run`,
update task state, broadcast `notifications/progress`. Pool size N is
configurable; default 1 (matches current api_mutex behavior, makes the
pool size lift trivial when P2-20260421-001 unblocks parallel
inference).

### Admission control

`entropic.ask` with `async=true` checks queue depth before spawning.
If full, returns:

```json
{ "error": "queue full",
  "code": "ENTROPIC_ERROR_LIMIT_REACHED",
  "queue_depth": 256,
  "queue_capacity": 256 }
```

Surface to MCP tool result as a structured error. Consumers can
choose to retry, back off, or fail upward.

### Backpressure policy (queue-full / subscriber-full)

Two distinct queues with two distinct policies:

| Queue | Default policy | Configurable? |
|---|---|---|
| Task queue (entropic.ask submissions) | **Reject** with explicit error | Yes — could become *block* (apply backpressure on client write) |
| Per-subscriber outbound queue | **Drop oldest** (preserve liveness) | Yes — could become *drop newest*, *disconnect subscriber* |

Document both in `external_bridge.h`. Add config knobs:

```yaml
bridge:
  task_queue_depth: 256
  task_workers: 1            # increases when api_mutex lifted
  subscriber_queue_depth: 256
  subscriber_overflow: drop_oldest  # drop_oldest | drop_newest | disconnect
  task_overflow: reject             # reject | block
```

## Constraints

- **api_mutex constraint.** Until P2-20260421-001 lifts the per-handle
  serialization, `task_workers` > 1 yields no real throughput gain.
  This proposal is therefore *primarily about correctness, bounded
  resource use, and admission control* — not throughput. Real
  parallel compute lands when the session-multiplexer proposal does.
- **Shutdown ordering.** The current bridge's `stop()` joins the
  accept thread; it doesn't track the detached async-task threads.
  Worker pool + per-subscriber writer threads need explicit
  shutdown: signal queues, drain or discard pending, join all
  threads. Adds a non-trivial state machine to `stop()`.
- **Memory.** Per-subscriber queue × N subscribers × queue depth × max
  message size ≈ bounded by config. Admission control on the task
  queue caps total in-flight work.

## Dependencies

- **P2-20260421-001** (Parallel Bridge Sessions) — independent, but
  this proposal's `task_workers > 1` only delivers throughput once
  P2-20260421-001 lifts api_mutex.
- v2.1.2 issue #4 fix (A+B+C+D) — ships first; this proposal
  *replaces* the broadcast/accept paths it lays down, doesn't extend
  them.

## Open Questions

1. **Pool size = 1 by default vs N.** Defaulting to 1 matches
   current behavior exactly; defaulting to N (e.g. number of cores)
   is forward-looking but allocates threads that block on api_mutex.
   Lean toward 1.
2. **Queue-full task overflow: reject or block.** Reject gives
   consumers a clear signal but requires error handling on every
   submission. Block applies natural backpressure (consumer's
   `entropic.ask` write blocks). Reject is the simpler default;
   block is the more "TCP-flavoured" semantic.
3. **Per-subscriber overflow: drop_oldest vs disconnect.**
   `drop_oldest` preserves liveness but loses early notifications.
   `disconnect` is loud; consumer reconnects. For
   `notifications/progress` specifically, drop_oldest is fine
   (consumer polls `ask_status` for state); for future structured
   notifications it might not be.
4. **Should the Python wrapper expose admission errors as a
   distinct exception type?** Currently `EntropicError` covers all
   error codes; `ENTROPIC_ERROR_LIMIT_REACHED` from a queue-full
   condition might warrant `BridgeQueueFullError` for ergonomics.

## Verification

1. **Submit 100 async tasks rapidly.** Assert: thread count stays
   bounded at `task_workers + accept_thread + N subscribers`.
2. **Slow subscriber test.** Connect a subscriber that doesn't
   read. Run 1000 broadcasts. Assert: subscriber's queue caps at
   depth, oldest entries dropped, broadcast call site never blocks
   for more than the queue-mutex hold time.
3. **Admission control test.** Fill task queue to capacity. Submit
   one more `entropic.ask`. Assert: returns `LIMIT_REACHED` error
   immediately, no thread spawned.
4. **Shutdown test.** Submit 10 tasks, half running, half queued.
   Call `stop()`. Assert: all threads join within deadline; queued
   tasks marked `cancelled` in registry; no leaked fds.
5. **FIFO ordering.** Submit tasks A, B, C with `task_workers=1`.
   Assert: completion notifications arrive in submission order.

## Out of Scope

- Lifting api_mutex (deferred to P2-20260421-001)
- Multiple engine handles per bridge (deferred to P2-20260421-001)
- Cross-bridge load balancing (not on the roadmap)
- Persistent task queue across restarts (rejected — restart loses
  in-flight work, consumers re-submit)

## Notes

This proposal was written 2026-04-29 during the v2.1.2 (issue #4)
session as the larger architectural follow-up to the focused deadlock
fix. The user explicitly scoped v2.1.2 to A+B+C+D (close the bug)
and this proposal (capture the broader rework) so neither slipped:
v2.1.2 ships in days, this proposal goes through normal review.
