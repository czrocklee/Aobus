---
id: development.naming-convention
---
# Naming conventions

This document is the source of truth for project-owned names.
Use it to choose vocabulary and roles; it is not a catalog of every noun that may appear in Aobus.
Mechanical placement rules, checker coverage, and automatic-proof boundaries are documented separately in [naming checks](lint/naming-checks.md).
Test names and Catch2 tags are owned by [test naming and assertions](test/naming-and-assertion.md).

## Principles

A name states a contract, owner, lifetime, or boundary.
Choose the most specific domain phrase that remains true for every supported case.
Prefer a plain domain noun when a role suffix adds no information.

Do not name a type after its storage shape, declaration grouping, current caller, or implementation technique when a domain contract exists.
Do not add a wrapper merely to obtain a role-shaped name.
Pure stateless behavior normally uses domain-prefixed free functions; state, identity, lifetime, or invariants justify a type.

Two names that describe the same contract and layer are not a reason for churn.
When both fit, use the narrower established term.
A name that misstates responsibility, ownership, lifetime, or layer should be fixed with the code that establishes the surviving contract.

External API, framework, protocol, file-format, and persisted vocabulary may remain at its boundary.
Translate it before it becomes project-owned API.

## Enforcement

Contributors choose semantic roles and vocabulary through review.
`./ao name-audit`, Aobus clang-tidy checks, and built-in `readability-identifier-naming` cover the mechanical and type-aware subset.
See [naming checks](lint/naming-checks.md#enforcement) for the exact audit inventory, checker names, framework proof, and template boundaries.
Do not turn semantic inference into a regex with exception churn.

## Identifier forms

Use `PascalCase` for types, classes, enums, scoped enum values, aliases, and concepts.
Use `camelCase` for functions, methods, parameters, locals, and passive struct fields.
Use `_camelCase` for non-static class data members.
Use `kCamelCase` for every `constexpr` variable and every namespace, static, or class constant, including a non-`constexpr` class constant.
An ordinary non-static local `const` remains `camelCase`.

Use full project vocabulary by default: `rowIndex`, `byteOffset`, `dictionaryId`, `transaction`, `argument`, and `metadata`.
Stable short forms are limited to:

- `id`, `ids`, `min`, `max`, `lhs`, and `rhs`;
- `config`, one-call context `ctx`, generic value `val`, generic string `str`, and generic `ao::Result` value `res`;
- tiny-loop `i` and `j`, iterator `it`, conversion `src` and `dst`, and argument-list `args`;
- storage handle `db`, temporary file `temp`, coordinate fields `x` and `y`; and
- chrono samples `tp` or `t0` through `tN`.

Prefer a concrete domain name whenever the value has a more specific role.
Review keeps short forms within this vocabulary; there is no general abbreviation checker.
Preserve external abbreviations only at their boundary.

Use normal acronym casing inside project names: `ResourceId`, not `ResourceID`.
Concepts use a capability name such as `Arithmetic` or `HasRawMethod`, without a `C` prefix or `Concept` suffix.
A single unconstrained template type may be `T`; constrained or role-bearing parameters use a descriptive `PascalCase` name.

Classes use underscored members; structs are passive aggregates with plain fields.
Because that distinction is API-visible, reconsider a struct that starts acquiring encapsulation rather than casually promoting it.
Use `cancelled` in project vocabulary and `canceled` only to match an external spelling.

### Pointer, optional, result, and time names

A `std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr`, or `Glib::RefPtr` variable ends in `Ptr`: `_storePtr`, `providerPtr`.
A raw pointer does not, because `Ptr` distinguishes an owning or counted handle from a plain observer: `_saveButton`, `targetWindow`.
References, spans, iterators, handles, callbacks, and value wrappers do not use `Ptr` either.
Do not use Hungarian prefixes such as `pWindow` or `_pWindow`.
A pointer-returning factory still describes what it creates: `makeRuntime()`, not `makeRuntimePtr()`.

Objective-C object pointers are a separate language boundary.
Under ARC, an ordinary object local or ivar is normally a strong owner and `__weak` identifies a non-owning target.
Keep native names such as `_window` without `Ptr`; the C++ observer interpretation does not apply.
Project Objective-C class names use an `Aobus` prefix because their runtime names do not use C++ namespaces.
Preserve framework selectors and the ownership semantics of the `init`, `new`, `copy`, and `mutableCopy` method families.
C++ declarations in the same `.mm` file continue to follow the C++ naming rules.

An `std::optional` variable begins with `opt`, for example `optTrackId`.
A function or type describes its semantic result and does not acquire that prefix.
Pointer nullability and expected error channels are not optionals.

An `ao::Result<T>` variable, parameter, or field is `res` when it has no more specific role, or a descriptive name ending in `Res`, such as `openRes`.
Class data members retain their underscore: `_res` or `_openRes`; passive struct fields use `res` or `openRes`.
`result` and `_result` are not generic-name exceptions.
A function returning `ao::Result<T>` describes its operation and does not acquire a `Result` or `Res` suffix merely because of its return type.

Chrono durations name the phenomenon rather than the storage unit: `timeout`, `elapsed`, and `retryDelay`.
Numeric representations include the unit: `timeoutMs`, `frameCount`.
Time points use `time`, `deadline`, `startedAt`, or `updatedAt`.
Reserve `timestamp` for serialized or protocol values.

## Choosing a role

Walk this decision in order and stop when one contract fits:

1. For passive data, choose its payload role from the table.
2. For authoritative state, retained derivation, ordered supply, or a bounded live conversation, choose `Store`, `Cache`, `Source`, `Projection`, `Service`, or `Session`.
3. For toolkit-neutral interaction state, choose a specific model role.
4. For a pipeline step, choose `Plan`, `Operation`, `Compiler`, `Evaluator`, `Builder`, or `Factory`.
5. For a boundary crossing or reshaping, choose `Adapter`, `Bridge`, `Provider`, `Reader`, `Writer`, `Importer`, `Exporter`, `Parser`, `Formatter`, or `Resolver`.
6. For a native surface, use the concrete frontend role.
7. Otherwise use a plain domain noun.
   Bare `Model` is a last resort, not a neutral suffix.

### Role table

| Family | Roles and contract |
| --- | --- |
| Authority and lifetime | `Service` owns an application/runtime side-effect boundary; `Session` owns a bounded active conversation; `Store` owns source-of-truth state or persistence access. |
| Derived reads | `Source` supplies ordered membership or a stream; `Projection` derives a read model; `Cache` retains invalidatable non-authoritative data. |
| UIModel state | `ViewModel` publishes UI-facing state and user actions; `InteractionModel` owns transient gestures; `EditorModel` or `FormModel` owns a draft, validation, and collection. Bare `Model` is used only when none is narrower. |
| Pure definition and choice | `Schema` defines valid structure; `Catalog` is a mostly static inventory; `Policy` makes deterministic decisions; `Recommender` chooses a preferred default. |
| User flow | `Workflow` is a stateless or short-lived multi-step user/business operation. It is not a subscription owner or generic helper. |
| Prepared execution | `Plan` describes computed work; `Operation` owns one stateful execution; `Compiler` lowers declarations; `Evaluator` executes a plan or rule. |
| Construction | `Builder` constructs incrementally; `Factory` selects an implementation family. A normal value uses a constructor or `make*` free function. |
| Boundary adaptation | `Adapter` reshapes an interface; `Bridge` crosses an external protocol/framework; `Provider` supplies a capability or backend. |
| Data boundaries | `Reader` and `Writer` perform boundary-scoped access; `Importer` and `Exporter` own durable formats; `Parser` converts syntax or bytes; `Formatter` creates presentation text; `Resolver` binds ids or references using context; `Codec` converts editable text and typed values both ways. |
| Passive values | `State` is passive current state; `Snapshot` is a point-in-time copy; `Config` is required value-only construction data; `Options` is optional knobs; `Spec` is requested shape; `Descriptor` is declared capability or metadata. |
| Inputs and results | `Dependencies` is construction-scoped collaborator wiring; `Context` is one-call non-owning input; `Request` crosses a service/process boundary; `Reply` is a synchronous domain response; `Outcome` classifies completion; `Result` summarizes completed work; `Progress` is in-flight data. |
| Frontend surfaces | `Widget`, `Dialog`, `Panel`, and `Component` own native or terminal presentation. `Controller` translates frontend events for one surface; `Coordinator` sequences multiple independent owners; `Host` owns placement/lifetime; `Bridge` adapts a platform protocol. |
| Test support | `Fixture` owns test lifetime; `TestSupport` is shared setup/assertion code; `Fake`, `Mock`, `Spy`, and `Stub` keep their ordinary test-double meanings. |

`View` is valid for a non-owning Core read view or for the product's workspace view concept.
It does not mean a native widget or a convenient bag of display state.
A toolkit-neutral display owner with actions is a `ViewModel`; a native surface uses a frontend role.

A bare `Editor` is a frontend surface.
Toolkit-neutral draft ownership is an `EditorModel` or `FormModel`; pure edit rules use domain functions, or a `Policy` only when they truly decide policy.
A cohesive capsule file may use an operation noun such as `Editing` or `Authoring` without inventing an owner type, but its declarations still expose concrete contracts.

Framework idioms remain local: GTK `*Object`, C++ `*Deleter` and `*Hash`, range `*Proxy`, and strong-type `*Tag` are not application roles.

## Files and ownership

A public file is a feature capsule, not necessarily a one-type container.
Several values and functions belong together when they share one owner, vocabulary, and change reason.
Do not merge unrelated declarations merely to reduce header count.

Name a file after its capsule or principal public contract.
Implementation files may partition a capsule by algorithm or platform without manufacturing a public header for every partition.
Tests are organized by behavior and need not mirror each implementation file.

Use singular feature directories.
Public UIModel declarations keep the flat `ao::uimodel` namespace; folder context does not excuse ambiguous public names.
Detailed allocation is owned by [UIModel organization](uimodel-organization.md).

Generic names such as `Common`, `Types`, `Utils`, and `Helpers` hide ownership.
Test, tool, or detail support may use a plural `Helpers` file only when no domain capsule is honest.
Do not create static-only production classes to simulate namespace scope.

When a consolidation changes a public type, rename its file, implementation symbols, variables, methods, tests, build entries, and documentation in the same change.
Make any compatibility or generated-file constraint explicit.

## Function names

Accessors name the value: `library()`, `rootPath()`, `trackCount()`.
Use `get*` only when the operation retrieves externally, performs meaningful work, or matches an external API.

A project-owned named function whose source contract fixes a direct `bool` result uses a predicate prefix: `is*`, `has*`, `can*`, `should*`, `supports*`, `needs*`, `matches*`, `accepts*`, or `covers*`.
An action that reports true on success uses `try*`; `handle*` and other generic action verbs are not blanket exceptions.
Do not use a bare adjective predicate.

Bool conversions are value APIs rather than predicates.
Exact `asBool` names a conversion in an `asString`/`asInt`/`asDouble` family, and exact `readBoolOr` names strict true/false scalar reading with a supplied fallback.
These do not create broad `as*` or `read*` exceptions.
Standard interface vocabulary remains valid: exact `empty`, `contains*`, `startsWith*`, and `endsWith*`, plus `compare_exchange_strong` and `compare_exchange_weak`.
A required foreign override or generated non-virtual API may retain its boundary spelling; use a narrow, check-specific `NOLINTNEXTLINE` with an adjacent explanation when the generated boundary cannot be proved automatically.
Rename an entire project-owned override chain rather than treating it as a framework exception.
The [naming-check reference](lint/naming-checks.md#bool-function-proof) defines the exact framework identities, source-fixed template proof, and diagnostic limits.

Every project-owned named function whose direct return type is `ao::async::Task<T>` ends in `Async`.
This includes infrastructure operations such as `sleepForAsync`, `whenAllAsync`, and `makeReadyTaskAsync`.
It does not apply merely because a function takes a task, or to lambda call operators and launchers returning `void`, `TaskHandle`, or `Future`.
A framework-fixed name remains unchanged.
`Async` means callers receive awaitable completion; it does not promise immediate start, a new thread, or an executor switch.
Template and deduced-return proof is documented in [naming checks](lint/naming-checks.md#async-function-proof).

Use verbs consistently:

- `find*` returns absence normally; `lookup*` queries a keyed authority; `resolve*` binds a reference with context; `require*` fails closed.
- `read*` and `write*` perform I/O or serialization; `load*` and `save*` cross persistence; `fetch*` retrieves remotely or asynchronously.
- `create*` makes a domain or persisted entity; `make*` constructs a C++ value; `build*` assembles a compound result.
- `prepare*` validates and stages; `execute*` performs prepared work; `apply*` applies a patch or decision; `commit*` makes staged work official; `publish*` makes state, events, or artifacts externally visible.
- `update*` changes from explicit input; `refresh*` rereads or recomputes; `reset*` returns to defaults; `clear*` removes current content; `remove*` removes membership; `delete*` deletes a domain entity.
- `open*` and `close*` own resource, session, or window boundaries; `start*` and `stop*` own ongoing activities. Prefer construction to `initialize*`; use it only for required post-construction framework setup.
- `collect*` traverses into a result; `filter*` produces or configures a filtered view; per-item predicates use `matches*` or `accepts*`; `merge*` includes conflict semantics; `combine*` does not.
- `show*` and `hide*` change visibility; `present*` brings a top-level surface forward; `reveal*` exposes an internal area; `dismiss*` closes a transient surface.
- `cancel*` is expected termination; `abort*` is forced termination; `reject*` refuses invalid or inadmissible input.

`on*` registers a subscription or names a stored callback slot.
Project-owned event processing uses `handle*`; producers use `emit*`, `notify*`, `post*`, or `enqueue*` according to the real mechanism.
Facts use past-tense suffixes such as `Changed`, `Added`, `Removed`, `Requested`, `Completed`, and `Failed`.

Use `register*` for catalogs, providers, types, or actions; `subscribe*` for observations with a lifetime handle; `connect*` only for signal or framework connections.
`dispatch*` requires real distribution or executor delivery.

Do not encode ordinary ownership with `own*`, `borrow*`, or `retain*`; types and signatures carry it.
`*Unchecked` is an internal validation escape.
Reserve `*Unsafe` for a real lifetime, threading, or security escape.

## Boundary vocabulary

Keep established boundary spellings only while at that boundary, for example LMDB `txn`, SPA/PipeWire `dict` and `param`, ALSA `params`, MP4 `meta` atoms, keyboard `Meta`, persisted keys, CLI flags, and binary-layout terms.
Translate them before they enter project-owned APIs.

As a verb, `layout` means UI measurement, allocation, or positioning.
A `*Layout` noun remains valid for binary-format field layouts and the application's declarative shell/layout domain.

## Review

Ask three questions about a proposed public role:

1. What does it own?
2. What does it guarantee beyond the types it contains?
3. Which correctness contract is lost if it is deleted?

If none has an answer, absorb the declaration into its real owner or use a function.
These are review questions, not a score or lint rule.
Fix unclear names and responsibilities instead of documenting one-off exceptions.
General comparison of alternatives belongs in [design review](design-review.md), product boundaries in the relevant [system topic](../system/README.md), and independently useful rationale in [decisions](../decision/README.md).
[Documentation maintenance](documentation.md) explains when a separate page helps.
