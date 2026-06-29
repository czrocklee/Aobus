# Async, coroutine, and runtime service reference

## Async and coroutine tests

Async tests must be deterministic.

Prefer:

- Immediate or queued executors controlled by the test.
- Explicit `runOne()` / `runUntilIdle()` progression.
- Barriers or captured callbacks to create known ordering points.
- `AsyncTestState` only as a bounded observation aid, not as the primary scheduler.

Avoid or minimize:

- `std::this_thread::sleep_for`.
- Repeated `std::this_thread::yield`.
- Polling loops in individual tests.
- Wall-clock time as proof of correctness.

If a timeout helper is necessary, keep it centralized and make failure diagnostics useful.

Example shape:

```cpp
runtime.spawnWithLifetime(&scope, task());
REQUIRE(executor.waitUntilQueued());

scope.cancel();
executor.runOne();

CHECK_FALSE(completed.get());
```

A better common executor API should expose operations like:

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

- create/post behavior.
- update behavior.
- dismissal/removal behavior.
- no-op behavior for missing IDs.
- feed/state projection.
- signal emission and non-emission.

## Callback tests

Callback assertions should be specific enough to reject wrong events:

```cpp
auto received = std::vector<NotificationId>{};
auto sub = service.onDismissed([&](auto id) { received.push_back(id); });

service.dismiss(id);
service.dismiss(NotificationId{999});

REQUIRE(received.size() == 1);
CHECK(received[0] == id);
```

Avoid only checking a boolean unless the contract has no payload.

## Subscriptions and lifetime

Keep subscriptions in named variables when their lifetime keeps callbacks connected:

```cpp
auto sub = service.onUpdated([&](auto id) { updatedId = id; });
```

For cancellation/lifetime tests, assert both sides:

- The cancelled work does not complete.
- No user-visible error is emitted unless cancellation is meant to surface.
- Queued callbacks after scope destruction are ignored safely.
