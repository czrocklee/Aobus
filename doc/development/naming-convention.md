---
id: development.naming-convention
type: development
status: current
domain: development
summary: Defines identifier shapes, role selection, vocabulary, and file naming policy.
---
# Naming conventions

This document is the source of truth for project-owned names.
It gives principles and a decision path; it is not a catalog of every noun that
may appear in Aobus.

Test names and Catch2 tags are owned by
[test naming and assertions](test/naming-and-assertion.md).

## Principles

A name states a contract, owner, lifetime, or boundary.
Choose the most specific domain phrase that remains true for every supported
case. Prefer a plain domain noun when a role suffix adds no information.

Do not name a type after its storage shape, declaration grouping, current
caller, or implementation technique when a domain contract exists.
Do not add a wrapper merely to obtain a role-shaped name.
Pure stateless behavior normally uses domain-prefixed free functions; state,
identity, lifetime, or invariants justify a type.

Two names that describe the same contract and layer are not a reason for churn.
When both fit, use the narrower established term. A name that misstates
responsibility, ownership, lifetime, or layer is not such a tie and should be
fixed with the code that establishes the surviving contract.

External API, framework, protocol, file-format, and persisted vocabulary may
remain at its boundary. Translate it before it becomes project-owned API.

## Enforcement

Naming rules have four enforcement levels.

**1. `./ao name-audit`** enforces mechanical file and placement rules:

- Banned catch-all file name suffixes: `Utils`, `Util`, `Utility`, `Types`.
- Singular `*Helper` file names are banned; plural support collections belong
  only in tests, tools, or implementation-detail areas.
- Layer placement for role suffixes: `ViewModel`, `Service`, `Component`, `Dialog`, `Widget`, `Panel`, `Controller`, `Coordinator`, `Host`, `Bridge`.
- `Fake*`, `Mock*`, `Spy*`, and `Stub*` types must live under `test/`.
- `*TestAccess` definitions are banned; use public behavior, constructor
  injection, or a production collaboration seam.

**2. Project clang-tidy checks** enforce type-aware identifier rules:

- `IdentifierNamingExtensionsCheck`: class members use `_camelCase`; passive
  struct fields use `camelCase`.
- `PointerNamingConventionCheck`: pointer-like variables use the `Ptr`
  rules below.
- `OptionalNamingAndUsageCheck`: optional values use the `opt` rules below.
- `ChronoNamingConventionCheck`: chrono values use unit-free time nouns.

**3. Built-in `readability-identifier-naming`** enforces the ordinary cases:

- `PascalCase` for types, enums, scoped enum values, aliases, and concepts;
- `kCamelCase` for constexpr values;
- `camelBack` for functions, methods, parameters, and locals; and
- an underscore prefix for private and protected data members.

GTK binding spellings such as `property_*`, `signal_*`, `vfunc_*`, and
`on_*` remain framework exceptions.

**4. Review** owns semantic role choice and vocabulary. Do not turn semantic
inference into a regex with exception churn.

## Identifier forms

Use full project vocabulary by default: `rowIndex`, `byteOffset`,
`dictionaryId`, `transaction`, `argument`, and `metadata`.
Stable short forms are limited to `id`, `ids`, `min`, `max`, `lhs`,
`rhs`, `config`, tiny-loop `i` and `j`, iterator `it`, conversion
`src` and `dst`, argument-list `args`, storage handle `db`, temporary
file `temp`, coordinate fields `x` and `y`, and chrono samples `tp` or
`t0` through `tN`.

Use normal acronym casing inside project names: `ResourceId`, not
`ResourceID`. Concepts use a capability name such as `Arithmetic` or
`HasRawMethod`, without a `C` prefix or `Concept` suffix.
A single unconstrained template type may be `T`; constrained or role-bearing
parameters use a descriptive `PascalCase` name.

Classes use underscored members; structs are passive aggregates with plain
fields. This distinction is API-visible, so a struct that starts acquiring
encapsulation should be reconsidered rather than casually promoted.

Constants use `kCamelCase`, including class-scoped non-constexpr constants.
Use `cancelled` in Aobus vocabulary and `canceled` only when matching an
external spelling.

### Pointer, optional, and time names

A `std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr`, or `Glib::RefPtr`
variable ends in `Ptr`: `_storePtr`, `providerPtr`. A raw pointer does not,
because the suffix is what distinguishes an owning or counted handle from a
plain observer: `_saveButton`, `targetWindow`. Neither does a reference, span,
iterator, handle, callback, or value wrapper. Hungarian prefixes are rejected on
both: no `pWindow`, no `_pWindow`. A factory returning a pointer still describes
what it creates: `makeRuntime()`, not `makeRuntimePtr()`.

An `std::optional` variable begins with `opt`: `optTrackId`.
A function or type name describes the semantic result and does not acquire that
prefix. Pointer nullability and expected error channels are not optionals.

Chrono durations name the phenomenon, not the storage unit:
`timeout`, `elapsed`, `retryDelay`. Numeric representations include the
unit: `timeoutMs`, `frameCount`. Time points use `time`, `deadline`,
`startedAt`, or `updatedAt`; reserve `timestamp` for serialized or
protocol values.

## Choosing a role

Walk this decision in order and stop when one contract fits.

1. If the declaration is passive data, choose its payload role from the table.
2. If it owns authoritative state, retained derivation, ordered supply, or a
   bounded live conversation, choose `Store`, `Cache`, `Source`,
   `Projection`, `Service`, or `Session`.
3. If it owns toolkit-neutral interaction state, choose a specific model role.
4. If it is a pipeline step, choose `Plan`, `Operation`, `Compiler`,
   `Evaluator`, `Builder`, or `Factory`.
5. If it crosses or reshapes a boundary, choose `Adapter`, `Bridge`,
   `Provider`, `Reader`, `Writer`, `Importer`, `Exporter`, `Parser`,
   `Formatter`, or `Resolver`.
6. If it is a native surface, use the concrete frontend role.
7. Otherwise use a plain domain noun. Bare `Model` is a last resort, not a
   neutral suffix.

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

`View` is valid for a non-owning Core read view or for the product's workspace
view concept. It does not mean a native widget or a convenient bag of display
state. A toolkit-neutral display owner with actions is a `ViewModel`; a native
surface uses the frontend roles above.

A bare `Editor` is a frontend surface. Toolkit-neutral draft ownership is an
`EditorModel` or `FormModel`; pure edit rules use domain functions or a
`Policy` only when they truly decide policy. A cohesive capsule file may use
an operation noun such as `Editing` or `Authoring` without inventing an
owner type, but its declarations must still expose concrete contracts.

Framework idioms remain local: GTK `*Object`, C++ `*Deleter` and `*Hash`,
range `*Proxy`, and strong-type `*Tag` are not application roles.

## Files and ownership

A public file is a feature capsule, not necessarily a one-type container.
Several values and functions belong together when they share one owner,
vocabulary, and change reason. Do not merge unrelated declarations merely to
reduce header count.

Name a file after that capsule or its principal public contract. Implementation
files may partition a capsule by algorithm or platform without manufacturing a
public header for each partition. Tests are organized by behavior; they need
not mirror each implementation file.

Use singular feature directories. Public UIModel declarations keep the flat
`ao::uimodel` namespace; folder context does not excuse ambiguous public
names. Detailed UIModel allocation is owned by
[UIModel organization](uimodel-organization.md).

Generic names such as `Common`, `Types`, `Utils`, and `Helpers` hide
ownership. Test/tool/detail support may use a plural `Helpers` file only when
no domain capsule is honest. Do not create static-only production classes to
simulate namespace scope.

When a consolidation changes a public type, rename its file, implementation
symbols, variables, methods, tests, build entries, and documentation in the
same change. A compatibility or generated-file constraint must be explicit.

## Function names

Accessors name the value: `library()`, `rootPath()`, `trackCount()`.
Use `get*` only when the operation retrieves externally, performs meaningful
work, or matches an external API. Boolean queries use `is*`, `has*`,
`can*`, `should*`, `supports*`, or `needs*`; avoid bare adjective
predicates.

Use verbs consistently:

- `find*` returns absence normally; `lookup*` queries a keyed authority;
  `resolve*` binds a reference with context; `require*` fails closed.
- `read*` and `write*` perform IO or serialization; `load*` and `save*`
  cross a persistence boundary; `fetch*` retrieves remotely or asynchronously.
- `create*` makes a domain or persisted entity; `make*` constructs a C++
  value; `build*` assembles a compound result.
- `prepare*` validates and stages; `execute*` performs prepared work;
  `apply*` applies a patch or decision; `commit*` makes staged work official;
  `publish*` makes state, events, or artifacts externally visible.
- `update*` changes from explicit input; `refresh*` rereads or recomputes;
  `reset*` returns to defaults; `clear*` removes current content;
  `remove*` removes membership; `delete*` deletes a domain entity.
- `open*` and `close*` own resource/session/window boundaries; `start*`
  and `stop*` own ongoing activities. Prefer construction over
  `initialize*`; use it only for required post-construction framework setup.
- `collect*` traverses into a result; `filter*` produces/configures a
  filtered view; per-item predicates use `matches*` or `accepts*`;
  `merge*` includes conflict semantics; `combine*` does not.
- `show*` and `hide*` change visibility; `present*` brings a top-level
  surface forward; `reveal*` exposes an internal area; `dismiss*` closes a
  transient surface.
- `cancel*` is expected termination; `abort*` is forced termination;
  `reject*` refuses invalid or inadmissible input.

`on*` registers a subscription or names a stored callback slot.
Project-owned event processing uses `handle*`; producers use `emit*`,
`notify*`, `post*`, or `enqueue*` according to the real mechanism.
Facts use past-tense suffixes such as `Changed`, `Added`, `Removed`,
`Requested`, `Completed`, and `Failed`.

Use `register*` for catalogs, providers, types, or actions; `subscribe*` for
observations with a lifetime handle; `connect*` only for signal/framework
connections. `dispatch*` requires real distribution or executor delivery.

Do not encode ordinary ownership with `own*`, `borrow*`, or `retain*`.
Types and signatures carry it. `*Unchecked` is an internal validation escape;
`*Unsafe` is reserved for a real lifetime, threading, or security escape.

## Boundary vocabulary

Keep established boundary spellings only while at that boundary, for example
LMDB `txn`, SPA/PipeWire `dict` and `param`, ALSA `params`, MP4
`meta` atoms, keyboard `Meta`, persisted keys, CLI flags, and binary-layout
terms. Translate them before they enter project-owned APIs.

`layout` as a verb means UI measurement, allocation, or positioning.
A `*Layout` noun remains valid for binary-format field layouts and the
application's declarative shell/layout domain.

## Review

Ask three questions about a proposed public role:

1. What does it own?
2. What does it guarantee beyond the types it contains?
3. Which correctness contract is lost if it is deleted?

If none has an answer, absorb the declaration into its real owner or use a
function. These are review questions, not a score and not a lint rule.

Prefer fixing an unclear name and its responsibility over documenting a
one-off exception. If a recurring decision is genuinely architectural, state
the general principle here; implementation-specific history does not belong in
this document.
