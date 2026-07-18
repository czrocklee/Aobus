---
id: rfc.0031.shared-signal-primitives
type: rfc
status: implemented
domain: async
summary: Moved generic signal and runtime subscription mechanisms into ao_async while preserving owner-affine delivery and weak deferred lifetime.
depends-on: rfc.0011.executor-affine-reporting-feed
---
# RFC 0031: Shared signal primitives

## Disposition

Implemented on 2026-07-18 with the narrow mechanism move described below.

The implementation:

- moves `Signal` and the shared `Subscription` vocabulary into public `ao_async` headers;
- records the public `ao_async -> ao_utility` dependency explicitly;
- preserves synchronous connection order, reentrant mutation, nested delivery, first-exception propagation, and weak deferred lifetime;
- migrates runtime, UIModel, GTK, TUI, and tests without changing subscription member order;
- deletes both runtime compatibility headers and guards against reintroducing their includes or qualified names;
- keeps `audio::Subscription` independent so `ao_audio` gains no unnecessary async dependency; and
- gives the mechanism focused core tests independent of `AppRuntime`.

The implemented canonical publication stages of RFC 0011 had already separated reporting revision queuing and observer-failure containment from the generic signal.
Its remaining retention and structured-content proposal stages do not alter this primitive, so they do not block this completed move.

`post` remains a member, `hasConnectedHandlers` remains owner-domain inspection, and the type keeps the concise name `Signal`.
No event bus, mutex, compatibility alias, or reporting-specific policy was added.

The [system architecture](../architecture/system-overview.md), [runtime execution architecture](../architecture/runtime-execution.md), [failure and reporting architecture](../architecture/failure-and-reporting.md), and [signal delivery specification](../spec/async/signal.md) own current behavior.
They supersede this proposal; this RFC retains the original problem, selected design, and rejected alternatives.

## Problem

At proposal time, `rt::Signal` and `rt::Subscription` were application-runtime names for mechanisms that contained no application-domain semantics.

`Signal` is a header-only reentrant observer list.
It depends on `async::Executor` only for deferred `post`, and it uses a weak shared state so a deferred emission becomes harmless after signal destruction.
`Subscription` is only an alias of [`utility::ScopedRegistration`](../../include/ao/utility/ScopedRegistration.h).

The current placement forces UIModel and frontend headers to include an `ao/rt` mechanism merely to own callback lifetime.
Dozens of runtime, UIModel, GTK, TUI, and test files include the two headers, and more than one hundred call sites explicitly spell `rt::Signal` or `rt::Subscription`.
The namespace therefore communicates false ownership and makes a reusable callback primitive appear to be an application service contract.

The move is mechanically large but semantically coupled to reporting.
`NotificationService` currently relies on several `Signal` instances whose ordered synchronous delivery and exception propagation contribute to the split-publication defect described by [RFC 0011](0011-executor-affine-reporting-feed.md).
Moving that use unchanged and then replacing it with a canonical feed publisher would cause avoidable migration churn and could accidentally promote reporting-specific behavior into the generic primitive.

The existing `audio::Subscription` alias is a separate core-audio vocabulary choice over the same utility guard.
Making `ao_audio` depend on `ao_async` solely to eliminate an alias would reverse a useful narrow dependency, so this RFC must distinguish the runtime/UIModel move from unrelated domain aliases.

## Dependencies

- Hard: [RFC 0011](0011-executor-affine-reporting-feed.md).
- Conditional: None.
- Integration: None.

RFC 0011 must first settle whether the reporting feed uses the generic signal at all and how observer exceptions, revision delivery, and reentrant publication are contained.
The rest of the repository can be inventoried earlier, but the complete public migration and deletion of `rt::Signal` cannot finish safely while that contract is unresolved.
Its implemented canonical publication stages settled those questions before this RFC was implemented; the remaining RFC 0011 stages concern retention, content, and discoverability instead.

## Goals

- Place the reusable signal mechanism in `ao_async`, below application runtime and UIModel.
- Expose one `async::Subscription` lifetime name backed by `utility::ScopedRegistration` for async/runtime/UIModel callback APIs.
- Preserve the existing owner-affine synchronous delivery, reentrant connection/disconnection, deferred-post lifetime, and exception behavior unless a separate accepted proposal changes them.
- Keep domain event values and service affinity contracts with their current owners; only the delivery mechanism moves.
- Migrate runtime, UIModel, GTK, TUI, and tests in one coherent source transition without long-lived compatibility aliases.
- Give signal behavior focused core tests independent of `AppRuntime` composition.
- Model the new `ao_async -> ao_utility` dependency explicitly in CMake.

## Non-goals

- Create a process-wide event bus, reflection registry, or named-topic system.
- Make `Signal` thread-safe or permit arbitrary cross-thread `connect`, `emit`, or disconnect operations.
- Replace callback-executor affinity with mutexes.
- Implement RFC 0011's revisioned reporting feed inside the generic signal.
- Replace every single-callback `std::function`, audio callback, GLib signal, or FTXUI event with `async::Signal`.
- Move application event payloads, reporting state, playback state, or UIModel policy into `ao_async`.
- Force `audio::Subscription` to migrate when doing so would add an otherwise unnecessary `ao_audio -> ao_async` dependency.

## Implemented design

### Core public surface

Move the mechanism to:

```text
include/ao/async/Signal.h
include/ao/async/Subscription.h
namespace ao::async
```

`async::Subscription` remains a semantic alias of `utility::ScopedRegistration`.
`ao_async` declares a public dependency on `ao_utility` so that the header dependency is represented by the target graph rather than relying on the umbrella target.

`async::Signal<Args...>` retains this public shape:

```text
connect(handler) -> async::Subscription
emit(args...)
post(executor, decayed args...)
hasConnectedHandlers()
disconnectAll()
```

The names describe mechanism behavior only.
Domain services continue to expose semantic methods such as `onStarted`, `onChanged`, or `subscribe`, and callers normally do not access the underlying signal object.

### Execution and thread-safety contract

`Signal` remains unsynchronized.
Construction, connection, synchronous emission, disconnection, and destruction occur on the serialized domain chosen by the owner.
For runtime services that domain is normally the callback executor defined by the [runtime execution architecture](../architecture/runtime-execution.md).

`post` is the only built-in executor hop.
It defers a decayed value payload to the supplied executor, retains only a weak reference to signal state, and emits only if that state still exists.
Calling `post` does not make later synchronous signal operations thread-safe and does not establish that the supplied executor is the owner's correct domain; the service contract remains responsible for that choice.

The primitive gains no mutex, worker, queue, or scheduler of its own.

### Reentrancy contract

The current behavior becomes an explicit core contract:

- one emission snapshots the handler count at its start;
- a handler connected during emission is first eligible for a later emission;
- disconnecting a not-yet-run handler during emission prevents that handler from running;
- disconnecting the active handler keeps its callable alive until the outermost emission returns;
- nested emission is permitted and compaction waits for the outermost emission;
- `disconnectAll` prevents remaining handlers from running; and
- destroying the signal invalidates every outstanding subscription and deferred post without dereferencing a destroyed owner.

These rules are mechanism behavior, not reporting-feed transaction behavior.

### Observer exceptions

Synchronous `emit` continues to invoke every still-connected handler, captures the first thrown exception, and rethrows it after delivery finishes.
The generic primitive does not log, swallow, normalize, or route exceptions because it has no diagnostic owner.

A domain that commits state before observation must put an explicit containment/publication layer above `Signal` when exceptions cannot escape the command.
RFC 0011 owns that requirement for the reporting feed.
Other services retain their current leaf or caller ownership unless their own specification changes.

### Subscription vocabulary

Runtime and UIModel public APIs return `async::Subscription` directly.
The old `rt::Subscription` alias is deleted after migration so a new application header does not continue the false ownership.

`audio::Subscription` may remain as a domain-facing alias to `utility::ScopedRegistration` because it is part of the independent audio provider API and avoids a new audio-to-async dependency.
Other resource guards that are not subscriptions continue to use role-specific names or `utility::ScopedRegistration`; this RFC does not rename all scope guards to `Subscription`.

### Migration sequence

1. Complete or accept RFC 0011's feed publication design and identify which reporting observers still need the generic primitive.
2. Add the `ao_async` headers and focused mechanism tests with the current behavior.
3. Change runtime public APIs and implementation internals to `async::Signal` and `async::Subscription`.
4. Migrate UIModel, GTK, TUI, and tests, updating direct includes and namespace qualifications.
5. Keep `audio::Subscription` independent and audit only accidental uses outside the audio provider boundary.
6. Delete the `ao/rt` headers and add a repository guard against reintroducing application-owned generic signal primitives.

The source migration is one atomic repository change after the new core tests pass.
Temporary aliases may exist inside that change but do not land as a supported compatibility surface.

## Alternatives

### Keep the types in application runtime

This avoids mechanical churn but preserves a false layer dependency in UIModel and frontends and keeps generic behavior coupled to reporting refactors.

### Move Signal into ao_utility

The observer list itself is generic, but its public `post` operation depends on `async::Executor`.
Putting the complete type in utility would either reverse the utility-to-async dependency or split one small contract across unrelated helpers.
`ao_async` is the narrower existing owner for callback delivery and executor lifetime.

### Remove Signal::post and use a free adapter

A free `post(executor, signal, args...)` could keep the base container in utility, but it exposes the signal object at more call sites and does not improve current ownership.
The weak-lifetime hop is already part of the primitive's useful contract.

### Use a third-party signal library

Replacing the implementation would require revalidating reentrancy, move-only handlers, exception delivery, deferred lifetime, and disconnect behavior for no demonstrated product benefit.

### Make Signal internally synchronized

A mutex cannot select the correct application executor and would make handler invocation and reentrancy substantially more dangerous.
Cross-thread producers must return through the owning service boundary instead.

## Compatibility and migration

The proposal changes only in-process source APIs and include paths.
It changes no persisted state, protocol, user-visible behavior, event ordering, or callback payload.

Because Aobus has no source-compatibility obligation between internal application layers, the old runtime aliases are removed rather than deprecated.
External code using public core audio provider APIs is unaffected unless a later audio-specific proposal changes its alias.

The migration must preserve subscription destruction order.
Mechanical type replacement is not allowed to move a subscription member relative to the service or view model it observes, because reverse member destruction may be part of callback teardown safety.

## Validation

- Core tests cover connection order, move-only handlers, connect-during-emit, disconnect-before-turn, self-disconnect, nested emit, disconnect-all, signal destruction before subscription, and subscription move assignment.
- Exception tests prove all still-connected handlers run and the first exception is rethrown after delivery.
- Deferred tests prove value decay/move behavior, later-turn execution, and harmless signal destruction before executor drain.
- Existing runtime/UIModel behavior tests pass without changing event counts or order.
- Header and target checks prove UIModel no longer includes `ao/rt/Signal.h` or `ao/rt/Subscription.h` and `ao_async` declares its utility dependency.
- A repository search finds no remaining runtime aliases or compatibility headers.
- Concurrency tests and ThreadSanitizer continue to prove producer-to-owner executor handoff; no test treats the primitive itself as thread-safe.
- The implementation passes `./ao check` and `./ao docs check`.

## Resolved questions

- `post` remains a member because the weak-state hop is part of the signal lifetime contract and a free adapter would expose the signal object without simplifying ownership.
- `hasConnectedHandlers` remains public as explicitly owner-domain-only inspection; it is not a synchronization or policy API.
- The core name remains `Signal`; `CallbackSignal` would add length without distinguishing another callback mechanism used by this API.

## Promotion plan

The [system architecture](../architecture/system-overview.md) and [runtime execution architecture](../architecture/runtime-execution.md) now record the lower-level callback mechanism and explicit owner-affinity rule.
The [failure and reporting architecture](../architecture/failure-and-reporting.md) records the final reporting-feed use selected by the implemented RFC 0011 publication stages.

The current [signal delivery specification](../spec/async/signal.md) owns ordering, reentrancy, exception, and deferred-lifetime behavior.
Public APIs now name `async::Subscription`, focused async signal tests appear in the test maps, and the application build owns the guardrail against restoring runtime signal primitives.
No separate decision record was needed because the implemented placement and preserved exception behavior match the accepted proposal without a new competing rationale.
