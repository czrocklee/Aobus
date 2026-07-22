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

- `InlineExecutor` only when callback turns and cross-thread behavior are explicitly out of scope.
- Production `LoopExecutor` when owner affinity and real turn semantics are the behavior under test.
- `ManualExecutor` or the Loop-backed test `QueuedExecutor` when a test needs one-step control, forced queuing, bounded waiting, or queue observations.
- Explicit `runOne()` / `runUntilIdle()` or `runOneTurn()` / `runReadyTurn()` progression.
- Barriers or captured callbacks to create known ordering points.
- `AsyncTestState` only as a bounded observation aid, not as the primary scheduler.

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
executor.expectQueued();
executor.runOne();
executor.runUntilIdle();
CHECK(executor.queuedCount() == 0);
```

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
- observer failure and reentrant FIFO delivery when those are part of the service contract.

## Callback tests

Callback assertions should be specific enough to reject wrong events:

```cpp
auto received = std::vector<NotificationFeedUpdate>{};
auto sub = service.onFeedUpdated(
  [&](auto const& update) { received.push_back(update); });

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

## Subscriptions and lifetime

Keep subscriptions in named variables when their lifetime keeps callbacks connected:

```cpp
auto latestFeed = std::shared_ptr<NotificationFeedState const>{};
auto sub = service.onFeedUpdated(
  [&](auto const& update) { latestFeed = update.feedPtr; });
```

For cancellation/lifetime tests, assert both sides:

- The cancelled work does not complete.
- No user-visible error is emitted unless cancellation is meant to surface.
- Queued callbacks after scope destruction are ignored safely.

For cross-thread and cancellation-race coverage, use
`concurrency-and-sanitizer.md`.
