---
id: decision.0016.compose-interactive-runtime
type: decision
status: accepted
domain: system
summary: Composes the interactive runtime over a hidden core runtime and rejects inheritance and service-location wiring.
---
# Decision 0016: compose the interactive runtime

## Context

The application originally modeled `AppRuntime` as a subclass of
`CoreRuntime`. That relationship made every protected core capability
implicitly available to the interactive layer, required virtual shutdown and
protected initialization hooks, and let frontend code treat the application
runtime as a raw storage owner. GTK's smart-list preview used that accidental
substitutability to reach `MusicLibrary` directly.

At the same time, frontend construction passed broad nullable dependency
aggregates through layout and component layers. The application-concept-debloat review considered
replacing those aggregates with a service locator or general dependency
container. That would reduce constructor spelling but keep each leaf's real
dependencies implicit and allow lifetime-invalid lookups.

The CLI needs the smaller storage/runtime graph without workspace, playback,
session persistence, or interactive resource caching. Interactive frontends
need those additional owners, but do not need polymorphic substitution for the
CLI composition.

## Decision

`AppRuntime` is a final composition root that owns exactly one
`CoreRuntime`. It is neither derived from nor convertible to the core owner
and never exposes that owner.

Its application-facing forwarding surface is explicit: library facade, async
runtime, source cache, notifications, completion, text ordering policy, and
music root. Raw `MusicLibrary`, database-path access, and core initialization
remain unavailable. Interactive resource-byte caching is an
`AppRuntime`-owned collaborator rather than a forwarded core service.

The core owner is declared before the interactive implementation owner so
reverse member destruction retires every interactive borrower before
`CoreRuntime`. Shutdown follows the same order explicitly.

`CoreRuntime` remains the composition for CLI and non-interactive workflows.
Both runtime roots publish typed-result factories and expose no partially
initialized object.

Frontend composition uses constructors and registration-time capture of exact
collaborators. Generation-scoped build contexts carry only values and
collaborators whose lifetime is bounded by that build. A general service
locator, runtime downcast, or retained nullable dependency bag is not part of
the application architecture.

## Alternatives considered

- **Keep inheritance and narrow call sites by convention.** Rejected because
  substitutability would still expose every current and future core member and
  make raw-storage access a review convention rather than a type boundary.
- **Protected inheritance.** Rejected because it preserves initialization and
  shutdown coupling while only changing which callers can exploit it.
- **Expose `core()` from a composed runtime.** Rejected because it recreates
  the same authority leak with one extra method call.
- **One universal runtime composition.** Rejected because CLI workflows would
  acquire interactive stores, playback, workspace, and cache lifetimes they do
  not use.
- **Service locator or dependency-injection container.** Rejected because leaf
  dependencies, availability, and lifetime would become runtime lookup facts
  instead of constructor facts checked by the compiler.
- **Broad typed dependency aggregates.** Rejected for retained wiring because
  optional fields and transitive bags obscure each consumer's actual
  capability set. A bounded per-call context remains valid when its fields all
  belong to that operation.

## Consequences

Frontend code can use only the audited application surface, and a new core
capability does not automatically become interactive API. The CLI keeps a
smaller graph. Smart-list preview and playback projection construction enter
through their runtime owners rather than through raw storage.

The composition root contains explicit forwarding methods and must update them
deliberately when a new cross-frontend capability is justified. Member order
and shutdown order are correctness constraints. Registration sites carry more
concrete constructor wiring than a locator would, but each component's
dependency and lifetime set is visible at compile time.

## Current authorities

- [System architecture](../architecture/system-overview.md)
- [Interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Application-layer review](../development/application-layer-review.md)

## Supersession

None.
