---
id: development.test.runtime-and-async
type: development
status: current
domain: development
summary: Defines deterministic asynchronous, coroutine, callback, and runtime-service testing.
---
# Runtime and asynchronous testing

## Async and coroutine tests

Async tests must be deterministic.

Prefer:

- `InlineExecutor` only for fully synchronous, owner-thread state tests.
  It captures its construction thread, reports `isCurrent()` truthfully, and rejects non-empty work submitted from another thread.
  Its `defer()` runs inline and reentrantly, so it does not model the later-turn ordering required of production executors.
  Direct foreign-thread submissions throw, but the same violation through a `noexcept` boundary such as `Signal::post()` terminates the process.
- Production `LoopExecutor` when owner affinity and real turn semantics are the behavior under test.
- `ManualExecutor` when a test needs one-task control. Producers may submit from any thread, but only its construction
  thread may call `runOne()` or `runUntilIdle()`.
- The Loop-backed `QueuedExecutor` when a test needs production-like owner affinity, forced queuing, bounded waiting,
  or queue observations.
- Explicit `runOne()` / `runUntilIdle()` or `runOneTurn()` / `runReadyTurn()` progression.
- Barriers or captured callbacks to create known ordering points.
- `AsyncTestState` only as a bounded observation aid, not as the primary scheduler.

Do not use `InlineExecutor` as a Runtime callback executor when the exercised path can hop to a worker, timer, audio
provider, or another producer thread and then return to the callback executor. Use `QueuedExecutor` and drain it from
the owner thread. In particular, drain until task completion before calling `TaskFuture::get()` when callback-executor
work is required to make that future ready.

Avoid or minimize:

- `std::this_thread::sleep_for`.
- Repeated `std::this_thread::yield`.
- Polling loops in individual tests.
- Wall-clock time as proof of correctness.

If a timeout helper is necessary, keep it centralized and make failure diagnostics useful.
`runLoopUntil()` provides the bounded test-only driver for a production `LoopExecutor`; do not add local polling loops for the same job.

Example shape:

```cpp
runtime.spawnWithLifetime(
  &scope,
  [&runtime](std::stop_token stopToken)
  { return task(&runtime, stopToken); });
REQUIRE(executor.waitUntilQueued());

scope.cancelAll();
executor.runOne();

CHECK_FALSE(completed.get());
```

`spawnCancellable()` and `spawnWithLifetime()` share the same cooperative
cancellation mechanism but expose different ownership:

- `spawnCancellable()` returns a `TaskHandle`; destroying or resetting that
  handle requests stop.
- `spawnWithLifetime()` registers the task with a `LifetimeScope`; destroying
  or cancelling the scope requests stop for every registered task.

Both accept a task factory rather than an already-created coroutine. Pass its
`std::stop_token` through executor hops, timers, and stop-aware worker
operations. This lets cancellation be observed before the factory body starts
and after every suspension point that can outlive the owner.

Controlled test executors expose operations like:

```cpp
executor.checkQueued();
executor.runOne();
executor.runUntilIdle();
CHECK(executor.queuedCount() == 0);
```

`AppRuntimeTestSupport::makeStateOnlyRuntime()` and `RuntimeLibraryTestSupport::makeStateOnlyLibraryChanges()` are the named opt-ins for tests that exercise only synchronous state.
A test that can start asynchronous work must construct the corresponding runtime state with an explicit executor and drive that executor according to the tested scheduling contract.

## Runtime service tests

Runtime service tests should read like service contracts.

Good patterns:

- Subscribe, trigger, assert payload.
- Mutate, read service state, assert exact result.
- Test no-op cases do not publish events.
- Store returned subscriptions in named variables when their lifetime keeps callbacks connected.
- Verify revision/version counters when they are part of the public state.

For notification-like services, cover:

- post and keyed create-or-update behavior.
- validation and capacity rejection without partial mutation.
- authoritative expiry and stale timer generations.
- feed/state projection.
- immutable update delivery and non-emission for unchanged keyed requests.
- the `noexcept` observer contract and reentrant FIFO delivery when those are part of the service contract.

## Callback tests

Callback assertions should be specific enough to reject wrong events:

```cpp
auto received = std::vector<NotificationFeedUpdate>{};
auto sub = service.onFeedUpdated(
  [&](auto const& update) noexcept { received.push_back(update); });

service.post(NotificationSeverity::Warning,
             "Device unavailable",
             NotificationLifetime::history());

REQUIRE(received.size() == 1);
CHECK(received[0].mutationKind == NotificationFeedMutationKind::Posted);
REQUIRE(received[0].feedPtr);
CHECK(received[0].id == received[0].feedPtr->entries.back().id);
CHECK(received[0].feedPtr->entries.back().message == NotificationMessage{"Device unavailable"});
```

Avoid only checking a boolean unless the contract has no payload.
Do not invoke `REQUIRE`, `CHECK`, or `FAIL` inside a `noexcept` callback: Catch2 may throw while abort-after is active.
Record the callback payload or outcome and assert after delivery returns.

## Subscriptions and lifetime

Keep subscriptions in named variables when their lifetime keeps callbacks connected:

```cpp
auto latestFeed = std::shared_ptr<NotificationFeedState const>{};
auto sub = service.onFeedUpdated(
  [&](auto const& update) noexcept { latestFeed = update.feedPtr; });
```

For cancellation/lifetime tests, assert both sides:

- The cancelled work does not complete.
- No user-visible error is emitted unless cancellation is meant to surface.
- Queued callbacks after scope destruction are ignored safely.

For cross-thread and cancellation-race coverage, use
`concurrency-and-sanitizer.md`.
