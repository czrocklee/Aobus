# Naming Conventions

This document is the source of truth for Aobus naming policy. It covers
identifier shapes, type and contract names, semantic vocabulary, file names, and
helper/support allocation.

Test case names and Catch2 tags are covered separately in
`doc/dev/testing/naming-and-assertions.md`.

## How To Use This Document

Start from what you are naming and read only the matching entry point:

- A class, struct, or file role: the decision order in *Choosing A Role*, then
  the role table in *Production Roles*.
- A data-carrying struct: the payload order in *Data Payload Roles*.
- A function or method: *Accessors And Predicates* for getters and predicates,
  *General Verb Allocation* for action verbs, then the themed verb sections.
- A variable, member, or parameter: *Identifier Shapes*, plus *Pointer Names*,
  *Optional Names*, or *Time And Duration Names* when the type matches.
- A word choice or abbreviation question: *Semantic Vocabulary* and *Boundary
  Vocabulary*.

Two meta-rules keep the rules cheap to apply. Both are about synonyms, not
contracts: they apply only when the competing names are equally specific,
describe the same contract, and imply the same layer and responsibility. A
name that misstates any of those is a correctness finding, never churn —
naming carries architectural meaning in this project, and these meta-rules are
not an exit from that judgment.

- **Tie-break:** when two allowed names both fit, pick the narrower or more
  specific one. Only a genuine tie between equally specific, contract-equivalent
  names is not worth escalating.
- **No churn:** renaming between such equivalent names within one role or verb
  family is not an improvement worth a diff.

The body of this document states durable principles. Concrete outcomes of past
reviews are precedents; they live in *Appendix: Precedents* and are removed
once the code and the principles make them redundant.

## Goals

- Prefer names that expose the domain concept, ownership, lifetime, or boundary.
- Keep project-owned names strict by default; fix unclear code names instead of
  growing one-off exception rules.
- Keep external or compatibility vocabulary at the boundary and translate it
  before it becomes project-owned API.

## Enforcement

Naming rules run at four levels. The first three are mechanical; the fourth is
review judgment. Keep mechanical checks free of semantic inference (see *Review
Practice*).

**1. `./ao name-audit`**, run as part of `./ao hygiene`, enforces the mechanical
file and class-name rules:

- Banned catch-all file name suffixes: `Utils`, `Util`, `Utility`, `Types`.
- Singular `*Helper` file names are banned; `*Helpers` files must live in
  test, tool, or detail implementation areas.
- Layer placement for role suffixes: `ViewModel`, `Service`, `Component`,
  `Dialog`, `Widget`, `Panel`, `Controller`, `Coordinator`, `Host`, `Bridge`.
- `Fake*`, `Mock*`, `Spy*`, and `Stub*` types must live under `test/`.
- `*TestAccess` definitions are banned in every layer; use public behavior,
  constructor injection, or a normal production collaboration seam instead.

**2. Project clang-tidy checks**, run by `./ao tidy` and on changed files by
`./ao hygiene`, enforce project-specific identifier rules:

- `IdentifierNamingExtensionsCheck`: `_camelCase` class members and plain
  `camelCase` struct members (see *Identifier Shapes*).
- `PointerNamingConventionCheck`: the `Ptr` suffix rules (see *Pointer Names*).
- `OptionalNamingAndUsageCheck`: the `opt` prefix rules (see *Optional
  Names*).
- `ChronoNamingConventionCheck`: time noun rules for `std::chrono` types (see
  *Time And Duration Names*).

**3. Built-in `readability-identifier-naming`**, configured in the shared
clang-tidy base and run by the same commands, enforces identifier case and
prefixes:

- `PascalCase` for classes, structs, enums, scoped enum constants, and type
  aliases.
- `kCamelCase` (the `k` prefix) for `constexpr` variables.
- `camelBack` for functions, methods, parameters, and local variables.
- The `_` prefix for private and protected members.
- GTK binding methods (`property_*`, `signal_*`, `vfunc_*`, `on_*`) are exempt.

  This check does not case-enforce non-`constexpr` class or static constants:
  only ignored-regexp keys are configured for them, so the `kCamelCase`
  constant policy in *Identifier Shapes* is review-enforced for those members.

**4. Review only**: role semantics, verb allocation, and vocabulary. These
carry architectural meaning and cannot be inferred mechanically.

## Identifier Shapes

- Types, classes, structs, enum classes, and scoped enum values use
  `PascalCase`: `TrackStore`, `Metadata`, `Code::IoError`.
- Functions and variables use `camelCase`: `loadMetadata()`, `trackCount`.
- Non-static class data members use `_camelCase`: `_handle`, `_tracks`.
- Struct data members use plain `camelCase`: `trackId`, `year`. This split
  makes the class-vs-struct choice API-visible: promoting a struct to a class
  renames its members. Keep structs strictly passive aggregates so that
  promotion stays rare.
- Constants use `kCamelCase`, whether class-scoped or unscoped: `kMaxSize`,
  `kDefaultFlags`.
- Use normal project acronym casing inside `PascalCase` names: `Id`, not `ID`;
  `Metadata`, not ambiguous `Meta`, unless matching a real external boundary.
- Concepts use `PascalCase` and are named as the capability or constraint they
  test: `Arithmetic`, `EnumType`, `HasRawMethod`, `PfrAggregate`. Do not add a
  `*Concept` suffix or a `C` prefix.
- Template parameters use `T` for a single unconstrained type, and a
  `PascalCase` role name when the parameter has a contract: `Setter`,
  `Scanner`, `PrimarySetter`.
- Namespaces are short lowercase words. Project code lives under `ao::` with
  established segments such as `rt`, `uimodel`, `library`, `async`, `audio`,
  and `query`; `detail` scopes internals. Public `uimodel` namespace policy is
  in *Uimodel Scope*.

## Type And Contract Names

- Type names describe the domain concept, role, or public contract. Do not name
  a type after storage shape or declaration grouping when a concrete domain name
  exists.
- Do not prefix project-owned contracts or abstract interfaces with `I`. Use the
  contract name for the public role, such as `Backend` or
  `TrackListProjection`, and give concrete implementations a semantic qualifier,
  such as `LiveTrackListProjection`.
- Do not define `*TestAccess` friend backdoors. Tests observe public behavior;
  when they need control over a collaborator, use a normal construction seam
  that is also usable by a production composition root.
- Avoid catch-all type and header concepts such as `*Types`, `*Common`, and
  generic `*Model` when the declarations can be split or named by a concrete
  concept such as capability, descriptor, binding, metadata, or decoded stream
  information.
- Use `*Count` for cardinality and `*Number` for domain ordinals.
- Use `indices` for positional-index collections. Use `indexes` only for
  database/search index entities.
- Recognize a small set of framework and C++ idioms at their boundaries. They
  are not architectural roles and do not enter the *Choosing A Role* order:
  `*Object` for a GTK `Glib::Object` subclass, `*Deleter` for a `unique_ptr`
  custom deleter, `*Proxy` for a C++ view or range proxy, `*Tag` for the
  phantom type parameter of a strong typedef, and `*Hash` for a hash functor.
  Keep each at the framework or standard-library boundary that motivates it.

## Class And File Role Names

Role suffixes are architectural vocabulary. Choose the narrowest suffix that
describes the type's contract; do not add a suffix because the type is near UI
code, holds state, or needs a nicer-sounding filename. When a role would need a
one-off exception, prefer renaming the type or moving the responsibility.
A role name is a contract, not a required suffix; prefer a plain domain noun
when it is clearer than any listed role.
Do not introduce static-only production classes to simulate namespace scope.
Use a real stateful type when there is state, identity, lifecycle, or
invariants; use domain-prefixed free functions for pure formatting, resolving,
conversion, or factory helpers.

### Choosing A Role

Work down this order and stop at the first match. The *Production Roles* table
is the canonical contract for each role; the sections after it add only layer
placement and nuance.

1. Test double or test scaffolding: use *Test And Support Roles*.
2. GTK or TUI surface: use *UI Roles* or *TUI Roles*.
3. Owns a live resource conversation, subscriptions, side effects, or a
   runtime workflow boundary: `*Service` or `*Session`.
4. Owns source-of-truth state or persistence access: `*Store`.
5. Derived read state over runtime/library state: `*Projection`; ordered
   membership or stream supply: `*Source`.
6. Retained, invalidatable derived lookup: `*Cache`.
7. Mostly data: pick by origin and validity using the payload order in *Data
   Payload Roles*.
8. A step in a prepare/execute pipeline: `*Plan`, `*Compiler`, `*Evaluator`,
   `*Builder`, `*Factory`, `*Operation`.
9. Reshapes or crosses an interface boundary: `*Adapter`, `*Bridge`,
   `*Provider`, `*Reader`/`*Writer`, `*Importer`/`*Exporter`, `*Parser`,
   `*Formatter`, `*Resolver`.
10. Behavior-bearing in-memory domain state with no narrower fit: the model
    roles, then bare `*Model` as the last resort (see *Model Roles*).
11. None of the above: prefer a plain domain noun over forcing a role suffix.

### Production Roles

| Suffix | Use | Avoid |
| --- | --- | --- |
| `*Model` | Rare fallback for behavior-bearing in-memory domain state after the narrower model roles and non-model roles do not fit. | Passive `State`, derived `Projection`, static `Policy`, formatter/resolver groups, schema records, or umbrella files. |
| `*InteractionModel` | Toolkit-neutral transient input or gesture state. | Runtime services, persistence, or long-lived application workflows. |
| `*EditorModel` | Stateful editor draft with options, validation, and a collect/build result. | Static editor decisions or one-shot factory functions. |
| `*FormModel` | Stateful form field collection with validation and patch/build output. | A single passive value or pure view state. |
| `*SessionModel` | Active in-memory application session state. | External resource ownership, which should use `*Session`, or passive snapshots. |
| `*State` | Passive snapshot or mutable state value. | Behavior-heavy objects or service-owned workflows. |
| `*Snapshot` | Point-in-time copy captured from a live source. | Mutable live state or persisted session payloads. |
| `*Cache` | Non-authoritative retained data or derived runtime objects keyed by lookup and invalidated for coherence. | Persistence, source-of-truth ownership, or registration APIs. |
| `*Config` | Required construction or initialization values; no collaborator objects. | Collaborator wiring, which should use `*Dependencies`; optional knobs, which should use `*Options`; or runtime-observed state. |
| `*Dependencies` | Construction-scoped collaborator bundle consumed to assemble an object or subsystem. | Value-only settings, which should use `*Config`; per-call inputs, which should use `*Context`; live mutable state; or a plural `*Services` name. |
| `*Context` | Passive, non-owning environment or input bundle for one call or bounded operation; not retained as wiring. | Construction wiring, which should use `*Dependencies`; ownership, lifecycle, subscriptions, or retained domain state; or a cross-cutting mutable bag. |
| `*Options` | Optional knobs that alter an operation or construction path. | Required identity, durable state, or result data. |
| `*Spec` | Declarative requested shape or desired configuration. | Observed runtime facts or persisted state. |
| `*Descriptor` | Static declared capability, action, property, or registration metadata. | Runtime observations, user music metadata, or mutable registry entries. |
| `*ViewModel` | UI-facing state plus user commands or presentation decisions; no GTK dependency or durable IO; belongs in `uimodel`. | GTK widgets, services, stores, or direct persistence logic. |
| `*Projection` | Derived read model over runtime/library state. | A mutable source of truth or UI-only state holder. |
| `*Source` | Ordered data, membership, or stream provider that backs projections or playback pipelines. | A generic owner, service boundary, or passive state value. |
| `*Service` | Runtime/app business boundary with side effects, async work, subscriptions, lifecycle, or cross-store coordination. | UI adapters, pure formatters, or small local helpers. |
| `*Store` | Source-of-truth state or persistence access boundary. | Ordinary caches, derived projections, or helper containers. |
| `*Catalog` | Mostly static declared inventory. | Mutable runtime lookup or registration state. |
| `*Registry` | Runtime mutable lookup or registration table. | Static descriptor lists. |
| `*Policy` | Pure decision rule. | IO, lifecycle, ownership, formatting, building, or long-lived mutable state. |
| `*Workflow` | A user/business flow with staged validation or multi-step state. | Plain helpers or one-shot actions. |
| `*Operation` | One-shot stateful execution object for applying prepared work with progress, cancellation, or scoped resources. | Pure evaluation, reusable services, passive plans, or ordinary helper functions. |
| `*Plan` | Computed work or execution description consumed by another step. | Mutable runtime state, service ownership, or result summaries. |
| `*Compiler` | Converts a declarative input into an executable plan, bytecode, or lowered representation. | Parsing alone or direct evaluation. |
| `*Evaluator` | Executes a plan, expression, or rule set against inputs. | Persistence, lifecycle, or source-of-truth ownership. |
| `*Executor` | Async execution context that runs submitted tasks on an owning thread or turn model (`dispatch`/`defer`); the primitive that other subsystems post work to. | One-shot prepared work, which should use `*Operation`; or a business boundary, which should use `*Service`. |
| `*Builder` | Incrementally constructs an object or aggregate. | Choosing implementations from a family. |
| `*Factory` | Chooses and creates implementations or resources from a family. | Ordinary pure value construction, which should use `make*` functions. |
| `*Reader` / `*Writer` | Transaction-scoped or boundary-scoped read/write access under a store or import/export workflow. | Long-lived runtime services or arbitrary file helpers. |
| `*Importer` / `*Exporter` | Durable format import/export boundary. | In-memory conversion or presentation formatting. |
| `*Formatter` | Creates presentation text. | Serialization or durable data conversion. |
| `*Parser` | Converts syntax or binary representation into structured values. | Contextual lookup or binding. |
| `*Resolver` | Binds names, ids, or references using context. | Searching a local list without binding semantics. |
| `*Adapter` | Reshapes one interface into another. | External protocol/framework integration when `Bridge` is clearer. |
| `*Bridge` | Adapts across an external protocol or framework boundary. | Ordinary intra-project forwarding. |
| `*Provider` | Supplies a concrete implementation, capability, or backend family. | Generic value accessors. |
| `*Session` | Owns a bounded active lifetime or conversation with a resource. | Passive state snapshots. |
| `*View` | Non-owning read view over core data, or a domain workspace view where `view` is the product concept. | GTK widgets, panels, pages, or view-models. |
| `*Request` | Input payload crossing a service, engine, process, or IPC boundary. | Local function argument bundles with no boundary meaning. |
| `*Reply` | Domain-level response from a synchronous command or mutation API. | Long-running operation summaries, which should use `Result`. |
| `*Outcome` | Enum classifying a discrete completion or disposition state. | Aggregate completion data, which should use `*Result`. |
| `*Result` | Operation outcome summary after work completes. | Exceptions/errors alone or command reply payloads. |
| `*Progress` | In-flight progress payload. | Final operation summaries or durable state. |
| `*Failure` | Structured recoverable failure payload. | Exception classes or generic error wrappers. |
| `*Event` | Internal queued or emitted event payload. | Passive state snapshots or final results. |
| `*Record` | Storage/log/foreign-system row or internally indexed entry. | Generic DTOs with no record identity or source. |
| `*Info` | Observed facts from a runtime, decoder, parser, or external source. | Static declared capabilities, which should usually be `Descriptor`. |
| `*Metadata` | User music metadata or real format/protocol metadata. | Generic descriptive fields for project-owned capabilities. |
| `*Actions` | A cohesive set of imperative user or application commands over a subsystem, as either a command interface or a free-function command module; holds no state. | Stateful glue, which should use `*Controller` or `*Coordinator`; a lookup or registration table, which should use `*Registry`; or an owning service. |

### Model Roles

`Model` is the broadest stateful role name and should be rare. Use a model
role only when the public contract owns in-memory state, exposes behavior over
that state, and no non-model role in *Choosing A Role* fits first. The table
above defines each model role; the rules here are placement and nuance only.

- `*ViewModel` belongs in `uimodel`, not in GTK code, services, stores, or
  durable persistence boundaries.
- `*SessionModel` is application session state. If the type owns an external
  resource conversation rather than only application state, use `*Session`.
- A framework adapter may use `*Model` when the external API itself uses model
  vocabulary, such as a GTK `Gio::ListModel` implementation. Keep that
  vocabulary at the adapter boundary.
- Bare `*Model` is a fallback for behavior-bearing domain state such as a
  keymap.

### Runtime And Core Roles

Runtime and core library code should not introduce `*Model` just because a type
owns state; the decision order in *Choosing A Role* names the boundary
instead. Placement and nuance:

- `*Store` may nest `Reader` and `Writer` types for transaction-scoped store
  access.
- `*Projection` can publish deltas, but it must not become the mutable source
  of truth.
- `*Source` can notify observers, but it is not a service boundary.
- `*Plan`, `*Compiler`, and `*Evaluator` split query/scan pipelines: compile or
  prepare a plan, then evaluate or apply it. Do not call a compiled plan a
  model.
- `*Operation` is appropriate when applying prepared work needs a bounded
  execution object for progress, cancellation, transactions, or scoped
  resources. Prefer a free function when the work is pure or stateless.
- `*Executor` names an async execution context that dispatches or defers tasks
  onto an owning thread or turn. It is infrastructure other subsystems post
  work to, not a service boundary and not a one-shot `*Operation`.
- `*View` is allowed in core storage for non-owning typed views, and in runtime
  workspace APIs where a user-visible workspace view is the domain object.
  GTK surfaces still use `Widget`, `Panel`, `Page`, `Dialog`, or `Window`.

### Data Payload Roles

Payload suffixes describe where the data comes from and how long it is valid.
Do not choose them by convenience or by field count. Decide in this order
before debating details:

1. Static declared data is `*Descriptor`.
2. Observed runtime or parsed data is `*Info`.
3. User music or real protocol/file facts are `*Metadata`.
4. Current observable values are `*State`.
5. Point-in-time copies are `*Snapshot`.
6. Construction and call inputs split by kind: required values are `*Config`,
   optional knobs are `*Options`, a requested target shape is `*Spec`, a
   construction-scoped bundle of collaborator objects is `*Dependencies`, and a
   passive non-owning environment bundle for one call or bounded operation is
   `*Context` (review-enforced: ownership and retention cannot be inferred from
   the suffix alone).
7. Operation-boundary data splits by phase: input `*Request`, synchronous
   response `*Reply`, discrete completion/disposition enum `*Outcome`, completed
   aggregate summary `*Result`, in-flight update `*Progress`, recoverable
   failure `*Failure`, queued/emitted payload `*Event`.
8. Persisted rows, log entries, external records, or registry entries with
   record identity are `*Record`.

Nuance beyond the table:

- `*State` may be mutable as a value object, but it should not own
  subscriptions, callbacks, threads, or resources.
- Session vocabulary splits three ways. `*Session` owns a bounded active
  resource conversation or lifetime (see *Production Roles*); `*SessionModel`
  holds in-memory application session state (see *Model Roles*); `*SessionState`
  is the persisted or restorable payload. Keep `*SessionState` stable enough for
  config/schema handling and separate from the live `*Session` and
  `*SessionModel` objects.
- `*Snapshot` is not a source of truth and should not imply persistence.
- Avoid project-owned `*Data` unless the external format or API itself uses
  that term. Prefer a domain noun plus one of the roles above.
- `*Dto` names a boundary payload owned by a CLI or serialization adapter. It
  stays inside that adapter and is translated with `to*Dto()` / `from*Dto()`
  (see *General Verb Allocation*) before it enters a domain or runtime API.
  Because it is explicit boundary vocabulary rather than a domain payload, it is
  exempt from the project-owned `*Data` warning above.

### UI Roles

- `*Panel` is a stable UI region or overlay surface. `Panel` is not a synonym
  for any grouped widgets.
- `*Widget` is a concrete GTK widget or composite widget. It should expose a
  widget contract, not application orchestration.
- Domain nouns such as `Control` may appear in a GTK type's stem when they are
  the natural UI concept, but the type still ends with a concrete UI role:
  `SeekControlWidget`, not `SeekControl`.
- `*Dialog`, `*Window`, `*Popover`, and `*Page` use their ordinary UI meanings:
  transient dialog, top-level window, transient popover, and navigable page.
- `*Controller` is imperative UI or framework glue that routes events,
  actions, widget lifecycle, or view/model binding. Do not use `Controller` for
  backend resource wrappers or domain services.
- `*Coordinator` orchestrates a multi-object workflow where no single
  controller or service should own the sequence.
- `*Component` is reserved for the declarative layout runtime's composable
  units. Ordinary GTK widgets are not components unless they implement the
  layout component contract.
- `*Host` owns and embeds a surface, page, layout runtime, or component tree.
- `*Actions` names a cohesive command set over a subsystem, expressed as a
  command interface (`ImportExportActions`) or a free-function command module
  (`PlaybackActions`). It carries no widget or state ownership; stateful glue
  stays in `*Controller` or `*Coordinator`.

### Uimodel Scope

- Public `uimodel` declarations use the flat `ao::uimodel` namespace described
  in `doc/design/uimodel-organization.md`; do not add feature-specific public
  subnamespaces for local grouping.
- Because folder context is not namespace context, public free functions in
  `uimodel` must carry their feature or domain context in the function name
  unless the parameter and return types already make the scope unambiguous:
  `makeSmartListDraft()`, not `makeDraft()`.
- In `uimodel`, avoid static-only scope classes especially aggressively because
  they obscure whether a name is a stateful view model, a passive view state, or
  pure presentation logic.

### TUI Roles

- `*InteractionModel` names toolkit-neutral transient TUI input state such as
  command palette, focus, selection, or prompt state.
- `*Entry` names passive selectable list/navigation rows in TUI presentation
  data. Avoid generic `*Item` for project-owned TUI data.
- `*Navigation` names construction and labeling of navigable TUI entry lists,
  such as library or track-presentation choices. It should not own selection
  state unless the type also has a stateful role name.
- `*Formatter` names display-text formatting. Use it for playback status,
  duration, and similar strings instead of collecting unrelated presentation
  helpers into one file.
- `*Style` names concrete visual styling values such as color/category mapping.
  It should not also build navigation entries or track rows.
- `*Section` names grouped track-table display data. Use a domain prefix such
  as `TrackSection`; do not hide section data in a broad presentation module.
- Do not add TUI umbrella presentation files that mix navigation, track rows,
  detail lines, selection movement, playback labels, and style mapping. Split
  by the concrete concept (see *Appendix: Precedents* for the recorded split).

### Test And Support Roles

- `*Fixture` owns test state or lifecycle for a test case, test file, or narrow
  behavior group. It should make arrange/setup explicit, not hide the behavior
  under test.
- `*TestSupport` groups reusable domain test scaffolding shared across multiple
  files or suites. Before adding one, search nearby test support headers and
  promote only genuinely common setup.
- `Fake*` is a working controlled implementation with state or simplified real
  behavior.
- `Mock*` is for interaction expectations.
- `Spy*` records calls, events, or state for later assertions without
  predeclaring expectations.
- `Stub*` provides fixed or minimal responses.
- `*Helpers` is allowed only for internal/detail/tooling free-function
  collections tied to a clear owner or domain. Do not use it as a domain or
  test junk drawer.
- Do not add domain test helper files named `*Utils`, `*Util`, or `*Utility`.
- The root `test/unit/TestUtils.h` header is reserved for low-level shared test
  utilities.

### File Names

- Production file names normally match the primary public type. A file that
  exposes a free-function role may still use a role noun such as
  `TrackFilterResolver.h`, but do not create a wrapper class just to satisfy a
  filename.
- When a file holds multiple public declarations, the filename must name the
  shared domain concept rather than a storage shape or a vague grouping word.
- A `*Model.h` file should expose a matching primary model type. Avoid leaf
  files named exactly `Model.h`; they usually hide schema records, presentation
  structs, or formatter functions that deserve a concrete concept name.
- Do not add new production `Utils`, `Util`, `Utility`, or `*Types` catch-all
  files. If a header exists only to gather unrelated declarations, split it; if
  the declarations share a real concept, name that concept directly.
- Directory context may supply part of the name. A local leaf such as `File.h`
  is acceptable when the parent directory is already the clear domain, such as a
  specific tag format directory.
- Tooling and adapter files may keep names that match external APIs,
  clang-tidy check ids, file formats, protocols, or user-visible compatibility
  surfaces.

## Pointer Names

Enforced by `PointerNamingConventionCheck` (see *Enforcement*).

- Managed pointers (`std::shared_ptr`, `std::unique_ptr`, `std::weak_ptr`,
  `Glib::RefPtr`) must end with the `Ptr` suffix: `trackPtr`.
- Raw pointers (`T*`) must not use the `Ptr` suffix. They represent non-owning
  observers, cursors, or iterators.
- The `Ptr` suffix rule applies to variables, fields, and parameters that hold
  pointer values. Raw-pointer-returning helper functions may use established
  names such as `asPtr()` when the function name describes a view/conversion
  contract rather than ownership.
- Do not use Hungarian notation for pointer types: avoid `pBuffer` and `_pRow`;
  use semantic names such as `bufferData` or `_activeRow`.

## Optional Names

Enforced by `OptionalNamingAndUsageCheck` (see *Enforcement*).

- `std::optional` variables, fields, and parameters must use an `opt` prefix:
  `optUri`, `optView`.
- Optional-returning functions should describe the domain rule, not the return
  container. Prefer `nonEmptyString()` or `dictionaryNameWhenPresent()` over
  names like `optionalString()`.
- Optional existence-check syntax is covered in `doc/dev/coding-style.md`
  because it is a C++ usage rule.

## Time And Duration Names

`std::chrono` types carry their unit and clock in the type, so names describe
the time concept, never the unit or the container. Enforced by
`ChronoNamingConventionCheck` (see *Enforcement*).

- `std::chrono::duration` variables, fields, parameters, and returning
  functions end with an approved duration noun: `duration`, `interval`,
  `timeout`, `delay`, `period`, `time`, `offset`, `position`, `threshold`,
  `elapsed`, `length`, `latency`, `remaining`, `budget`, `span`, `age`, or
  `delta`.
- `std::chrono::time_point` names end with an approved instant noun: `time`,
  `timestamp`, `instant`, `deadline`, `point`, `epoch`, `mark`, `start`,
  `end`, `expiry`, `origin`, or `now`. Local instant samples may use the
  conventional short forms `tp` and ordered `t0`, `t1`, ... when measuring an
  elapsed span between successive instants.
- Do not stack two time nouns: `elapsed`, not `elapsedDuration`; `now`, not
  `nowTime`; `timeout`, not `timeoutDuration`. Positional and descriptive
  compounds such as `startTime` or `bufferedDuration` remain idiomatic.
- Do not append unit tags such as `Ms` to chrono-typed names; the type carries
  the unit.
- Conversion and factory functions (`from*`, `to*`, `as*`, `make*`, `parse*`,
  `convert*`) are named after their input or intent and are exempt from the
  return-noun rule.

## Function And Method Names

### Accessors And Predicates

- Project-owned functions and methods use `camelCase`.
- Ordinary value and property accessors use the domain noun: `title()`,
  `trackIds()`, `trackCount()`, `currentTrack()`.
- Do not use `get*` just because a function returns a value. Reserve `get*` for
  framework or third-party signatures, established container/dictionary lookup
  vocabulary, compound acquisition such as `getOrCreate()` or
  `getOrDefault()`, and low-level interop handles where `get` is the external
  API vocabulary.
- Side-effect-free boolean queries must use predicate prefixes such as `is*`,
  `has*`, `can*`, `should*`, or `contains*`. Prefer `isEnabled()`,
  `isValid()`, `isEditable()`, `isEmpty()`, and `isEditing()` over
  `enabled()`, `valid()`, `editable()`, `empty()`, and `editing()`, except when
  matching an external boundary or standard-library-compatible API.
- Matching and capability predicates may use precise semantic verbs:
  `matches*` and `accepts*` are allowed for per-item matching, filtering, or
  visitor predicates, and `supports*` is allowed for capability support. Avoid
  widening that exception to ordinary state checks: prefer `is*Required`,
  `shouldBlock*`, `canAccept*`, or `is*Allowed` over `requires*`, `blocks*`,
  `accepts*`, or `allows*` for non-matching boolean queries.
- Boolean-returning actions keep action verbs when the return value reports
  success, change, consumption, or completion: `applyPatch()`,
  `writeTrackFieldPatch()`, `bind()`, `goBack()`.
- Return one value with a singular noun: `trackId()`, `xPosition()`. Return a
  collection with a plural noun: `trackIds()`, `xPositions()`. Counts use a
  singular target: `trackCount()`, not `tracksCount()`.
- Name ordinary collection values and collection-returning accessors with
  plural domain nouns: `trackIds`, `tracks`, `xPositions`, `devices`.
- Do not add storage-shape suffixes such as `List`, `Vector`, `Array`, `Map`,
  or `Set` merely to repeat the container type. Prefer `devices` over
  `deviceList` or `deviceVector`.
- Reserve `*List` for domain list entities, user-visible lists, query/list
  syntax, or framework/UI list concepts: `ListView`, `ListStore`, `SmartList`,
  `ListExpression`, `Gtk::ListBox`, `Gio::ListModel`.
- `*Map` and `*Set` are allowed when lookup or membership semantics are part of
  the local contract, not just an implementation detail: `nodeFormatMap`,
  `sinkCapabilitiesMap`, `reachableSet`.
- Use singular targets for single-item operations and plural targets for batch
  operations: `removeTrack(id)` versus `removeTracks(ids)`.

### General Verb Allocation

Use the narrowest verb that describes the observable contract. The meta-rules
in *How To Use This Document* apply here most often: when two verbs in one
family both fit, pick the narrower one and stop.

| Verb family | Use |
| --- | --- |
| `make*` / `create*` / `build*` | `make*` constructs pure in-memory values or objects. `create*` creates persistent, registered, externally visible, or owned resources. `build*` assembles derived plans, trees, projections, or aggregates from existing inputs. Do not decide by return type. |
| `new*` | Do not add project-owned `new*` functions or methods. Use `make*` or `create*`. |
| `load*` / `read*` / `parse*` | `load*` materializes domain objects or application state from durable sources. `read*` consumes bytes, records, fields, streams, or current state. Low-level helpers such as `readFile()` or `readFileBytes()` may return raw file contents; `load*` is for materialized domain values such as registries, layouts, tracks, or sessions. `parse*` converts syntax, text, or binary representation into structured data. |
| `decode*` / `encode*` | `decode*` converts an encoded media or binary representation into raw or structured media data; `encode*` performs the reverse. Contrast syntax-oriented `parse*` and storage- or wire-oriented `serialize*`. |
| `resolve*` / `find*` / `lookup*` | `resolve*` binds an id, name, or reference using context. `find*` searches locally and may not find a match. `lookup*` queries a table, catalog, schema, dictionary, or registry. |
| `write*` / `serialize*` / `emit*` / `dump*` | `write*` writes into a target object, file, storage, node, patch, or buffer. `serialize*` converts to storage or wire representation. `emit*` produces structured output, signal payload, or event text. `dump*` is diagnostic or user-requested raw/plain output. |
| `export*` / `import*` | Use for full transfer workflows across a boundary. |
| `to*` / `from*` / `as*` | `to*` creates a new representation. `from*` constructs or restores from a representation. `as*` returns a view, coercion, or access wrapper and must not imply ownership or expensive conversion. DTO helpers use `to*Dto()` and `from*Dto()`, not bare `*Dto()`. |
| `validate*` / `check*` / `ensure*` / `require*` | `validate*` checks business or format rules and returns a validation result or `Result`. `check*` is for tests, tools, diagnostics, or local consistency checks. `ensure*` establishes a postcondition and may create, initialize, repair, or error. `require*` enforces a mandatory precondition. |
| `try*` | Use only for expected failure or not-applicable paths that are not errors. True errors use `Result<T>` and a precise action name. |
| `compute*` / `calculate*` / `derive*` / `estimate*` / `measure*` | `compute*` deterministically derives a value without durable state changes. `calculate*` is numeric, geometric, time, size, or formula work. `derive*` infers domain meaning from source data. `estimate*` is approximate or heuristic. `measure*` observes external or runtime state. |
| `map*` / `translate*` / `convert*` / `adapt*` | `map*` is for key, id, index, or element mapping. `translate*` crosses namespaces, coordinate systems, protocols, or UI/domain boundaries. `convert*` changes type or representation, but prefer `to*` and `from*` when they fit. `adapt*` reshapes behavior or data for an interface, backend, or framework boundary. |
| `copy*` / `clone*` / `duplicate*` / `snapshot*` | `copy*` copies data to a target. `clone*` creates an equivalent independent object, especially owned or polymorphic objects. `duplicate*` is domain-level copying that creates a new identity or sibling. `snapshot*` captures static current state, not a live view. |
| `save*` / `store*` / `persist*` / `cache*` | `save*` is a user or business save workflow. `store*` places a value into storage, a container, a key-value backend, or a lower-level target. `persist*` guarantees cross-session or cross-process durability. `cache*` writes or maintains invalidatable derived data, not the source of truth. |
| `fetch*` / `request*` / `receive*` / `send*` / `post*` | `fetch*` actively obtains data from an external system, backend, remote, or cache-miss source. `request*` asks another layer to perform work. `receive*` passively accepts incoming data or events. `send*` transmits messages, commands, or data outward. `post*` enqueues asynchronous work or messages. |
| `print*` / `log*` / `report*` / `trace*` | `print*` writes directly to CLI/stdout/stderr or a stream. `log*` writes to the logging system. `report*` creates or submits aggregated diagnostics or results. `trace*` is low-level instrumentation. |
| `format*` / `describe*` / `label*` / `*Text` / `toString()` | `format*` creates presentation strings with formatting policy. `describe*` creates longer human or diagnostic text. `label*` creates short UI label text. `*Text` names existing text properties or payloads. Avoid `*String` names that only repeat the return type. Reserve `toString()` for generic enum or value conversion, not domain presentation formatting. |
| `diagnose*` / `explain*` | `diagnose*` analyzes problems and produces diagnostics or a report. `explain*` creates human-facing reasons; plain error text should usually use `format*` or `describe*`. |
| `process*` / `run*` / `execute*` | Avoid `process*` unless the function is truly a batch, stream, event-queue item, or work-item pipeline. Use `run*` for commands, tasks, tests, workflows, and loops. Use `execute*` for commands, instructions, actions, or executor contexts. Do not add `do*` or `perform*`. |
| `prepare*` / `configure*` / `finalize*` / `complete*` / `finish*` | `prepare*` establishes prerequisites for a later action. `configure*` sets options, policy, callbacks, or dependencies. `finalize*` ends a builder, serialization, transaction, or other phased flow and produces final state. `complete*` marks or advances work to success. Avoid new `finish*`; use `complete*`, `finalize*`, `close*`, or `commit*` when one is more precise. |

- Empty fluent builder factories use `makeEmpty()`, not `createNew()`.
- Fluent builder mutators use domain property names even when the operation has
  setter semantics: `name("Favorites")`, `duration(value)`, `smart(true)`.
  Reserve predicate forms such as `isSmart()` for boolean getters. Use `set*`
  for direct setters on ordinary mutable objects outside fluent builders.

### State, Lifecycle, And Time

- `set*` directly sets state or a property. `update*` computes, refreshes, or
  advances based on current state. `apply*` applies a patch, spec, event,
  decision, or state to a target. `refresh*` pulls, recomputes, or synchronizes
  current view/cache state.
- `reset*` returns something to default, initial, or empty state. `clear*`
  removes current content. `remove*` removes an item from a collection or
  parent. `delete*` deletes a persistent or domain entity. `erase*` is
  low-level container, storage, or STL-like removal. `toggle*` flips state; use
  `set*` when the target state is known.
- `open*` and `close*` are for resources, sessions, streams, files, database
  handles, and windows. `start*` and `stop*` are for ongoing activities,
  workers, timers, playback, or subscriptions. `pause*`, `resume*`, and
  `restart*` keep their ordinary lifecycle meanings.
- `begin*` starts a session, transaction, edit operation, or phase with a clear
  lifecycle. `end*` ends such a session or phase without implying success.
  `enter*` and `exit*` are for modes, states, and scopes.
- `initialize*` is the full spelling. Prefer constructors or factories over
  two-phase initialization; use `initialize*` only for explicit
  post-construction framework or external setup. Test arrangement may use
  `setup*`.
- `wait*` blocks for a condition, event, thread, or future. `await*` is only for
  coroutine or future-style async APIs. `poll*` checks external state or a queue
  without blocking. `tick*` drives one clock, timer, runtime, game-loop, or test
  scheduler step. `advance*` moves time, cursors, iterators, playback position,
  or state machines forward.
- State accessors use precise state words: `current*` is current context,
  `active*` is engaged/running state, `selected*` is selection model state,
  `focused*` is input/navigation focus, and `default*` is fallback or
  configured default value. Boolean predicates use `hasCurrent*()`,
  `isActive()`, `isSelected()`, `isFocused()`, and `isDefault()`.

### Events, Callbacks, And Subscriptions

- `on*` names a subscription API or callback slot from the caller's point of
  view: `playback.onStarted(handler)` means "run this handler when playback has
  started". Do not use `on*` for project-owned methods that process an incoming
  event.
- `handle*` names an event, framework signal, callback, command, or request
  processing entry point inside the receiving object:
  `handleSaveClicked()`, `handleTrackChanged()`,
  `handleRevealTrackRequested()`.
- `process*Event` may name consumption of a queued event work item when that
  distinction matters. The producer should still use `emit*`, `notify*`,
  `post*`, or `enqueue*` according to what it actually does.
- Pure C or framework callback thunks that only cast opaque user data and
  forward to a project method should usually be non-capturing lambdas at the
  registration site. Named callback entry points with real logic use `handle*`.
  A stored callable slot may still be named `on*` when it represents "call this
  when the fact happens".
- `notify*` notifies subscribers or services. `emit*` produces a signal,
  output, or event payload. Replace project-owned `fire*` with `emit*`.
- Use `dispatch*` only for real distribution to multiple handlers, a queue, or
  an executor.
- `subscribe*` creates an observer or callback subscription, preferably with a
  lifetime handle. `unsubscribe*` is explicit unsubscribe when RAII cannot cover
  it. `connect*` and `disconnect*` are for signal/slot, framework, or external
  connections. `register*` and `unregister*` are for long-term registries,
  catalogs, descriptors, capabilities, providers, types, or actions.
- Use `*Callback` for callable parameters or members, `*Handler` for event,
  command, or request processing behavior, and `*Observer` for state-change
  subscriptions. Do not add project-owned `*Listener` or `*Delegate`; keep those
  names only at third-party or framework boundaries.
- Use `before*` for pre-operation hooks and `after*` for post-operation hooks.
  Do not add project-owned `will*` or `did*`.
- Event/fact names use factual suffixes after the fact: `*Changed`, `*Added`,
  `*Removed`, `*Requested`, `*Completed`, and `*Failed`. Prefer
  `emitTrackChanged()` to produce an event, `onTrackChanged(handler)` to
  register a handler, and `handleTrackChanged()` to process the event inside a
  receiver.
- Widget and framework signal handlers should include the concrete signal fact
  when it is not already clear: `handleSaveClicked()`,
  `handleDeleteListActivated()`, `handleEditorSaveRequested()`,
  `handlePointerEntered()`, and `handleSelectionChanged()`.

### Collections, Matching, And Traversal

- `add*` adds membership without emphasizing position. `insert*` inserts at an
  index, iterator, or before/after location. `append*` and `prepend*` insert at
  the end or beginning. `replace*` substitutes a value or item. `swap*`
  exchanges two existing values or items. `push*` and `pop*` are only for
  stack, queue, buffer, or low-level container-like APIs.
- `move*` moves existing items while preserving identity. `reorder*` changes a
  batch order, usually from a complete order or spec. `sort*` orders by a key or
  comparator; do not use it for manual drag reordering. `rank*` computes ranks
  or priority and does not necessarily mutate order.
- `collect*` traverses sources, nodes, or items and returns a result set.
  `accumulate*` performs actual accumulation or reduction. Do not add
  `gather*`; use `collect*` or a more specific verb.
- `filter*` returns a filtered collection/view or configures a filter. A
  per-item predicate uses `matches*`, `accepts*`, `contains*`, or another
  predicate name. `compare*` is for ordering, diffing, or comparison reports;
  ordinary equality should use `operator==` or a domain-specific predicate such
  as `hasSameIdentityAs()`.
- `visit*` is for visitor pattern entry points or visiting one node/item.
  `walk*` recursively or sequentially walks a structure. Use `traverse*` and
  `iterate*` only when their algorithmic meaning is important; otherwise prefer
  `collect*`, `build*`, `find*`, `resolve*`, or `lookup*`.
- `merge*` combines same-kind data and handles conflicts, overrides, or
  priority. `combine*` forms one result from multiple inputs without conflict
  semantics. `compose*` combines behavior, functions, operations, or UI pieces.
  `join*` is for strings, paths, collections, or thread joins. `split*` divides
  one input into parts; parsing should still use `parse*`.

### UI, Library, And Playback Vocabulary

- `show*` and `hide*` only change visibility. `present*` brings an existing or
  creatable top-level window/dialog into the user's view. `reveal*` expands or
  exposes an internal UI area or state. `dismiss*` closes transient UI without
  implying resource destruction. Avoid `display*` as a generic verb.
- `focus*` is only for input or navigation focus. `activate*` and
  `deactivate*` enter or leave active state. `enable*` and `disable*` change
  capability or availability, not visibility or selection.
- `render*` creates visual representation or renderable data. `draw*` and
  `paint*` are low-level drawing callbacks or drawing-context operations.
  `layout*` as a verb is only UI measure, allocation, or child positioning; do
  not use it for domain metadata or schema. This restricts the verb only:
  fixed-field `*Layout` type nouns remain valid boundary vocabulary for
  binary-format field layouts (see *Boundary Vocabulary*). `arrange*` changes
  item order or placement.
  `position*` is for coordinates and placement, not generic order.
- `browse*` starts browsing UI or an external browser. `choose*` is explicit
  user choice from a dialog or options. `pick*` is internal or heuristic
  selection from candidates. `select*` mutates selection state or a selection
  model.
- `scan*` actively traverses external sources such as a filesystem or library
  source. `discover*` finds previously unknown resources, capabilities,
  devices, or entry points. `index*` builds or updates searchable indexes or
  catalogs for known items. `sync*` aligns state sources and should make
  direction clear when one-way. `watch*` continuously observes external
  changes.
- Playback actions use `play*`, `pause*`, `resume*`, `stop*`, `seek*`, and
  `scrub*` with their audio meanings. Read-only playback state uses predicates
  and accessors such as `isPlaying()` and `playbackPosition()`.

### Resource, Error, And Control Flow Vocabulary

- `cleanup*` is for test or local temporary-resource cleanup. Production APIs
  should prefer concrete verbs such as `close*`, `stop*`, `unsubscribe*`,
  `remove*`, or `delete*`. `destroy*` explicitly ends object/resource
  lifetime. `dispose*` is only for framework or binding vocabulary. `release*`
  releases ownership, handles, locks, or resources and must have a clear
  postcondition. `free*` is only for C/manual-memory boundaries.
- `commit*` makes staged, transactional, or edit-session changes official.
  `rollback*` undoes uncommitted changes within that transaction/session.
  `submit*` sends a request, command, form, or task to another layer.
  `publish*` makes state, artifacts, events, or results externally visible.
- `undo*` and `redo*` are undo-stack operations. `revert*` returns to a previous
  known or source-of-truth state. `restore*` recovers from a backup, snapshot,
  or persisted state.
- `lock*` and `unlock*` are for mutexes, resources, or explicit locked state.
  `guard*` creates or uses a guard object for scope, invariants, or reentrancy.
  `protect*` is security, permission, or protection vocabulary, not ordinary
  mutex or null-check vocabulary.
- `cancel*` is an expected cancellation of pending or ongoing work. `abort*` is
  forced or abnormal termination. `fail*` marks an operation, test, or result
  failed. `reject*` refuses a request, command, or input for validation,
  permission, or domain reasons. `skip*` deliberately skips and continues.
  `ignore*` deliberately discards; prefer `skip*`, `reject*`, or `cancel*`
  when they are more precise.
- Error and warning names are nouns or accessors such as `errorMessage()` and
  `warningCount()`, not action verbs. Result accessors use `hasError()`,
  `error()`, and `errorMessage()`, not `getError()`.

### Modifiers, Ownership, And Test Vocabulary

- `*IfNeeded` is only for idempotent guard-style actions. `*OrThrow` is
  discouraged; project-owned APIs should prefer `Result<T>`. `*Unchecked`
  means validation or precondition checks are deliberately skipped and should be
  private or internal. `*Unsafe` is only for explicit lifetime, threading, or
  security escape hatches. Avoid `*Internal` in public names; use privacy,
  `detail`, or a more specific name. `*Impl` is only for implementation hooks,
  pimpls, or backing helpers, not public/domain API.
- Do not encode ordinary ownership with `own*`, `borrow*`, or `retain*`
  functions. Use types, signatures, and member names to express ownership and
  lifetime. Keep `retain*` only at ref-counted or framework boundaries.
- Use prepositions only when they disambiguate. `*For*` names target,
  purpose, or context. `*By*` names a lookup, sort, group, or filter key.
  `*With*` names meaningful extra options or inputs. `*At*` names index, time,
  or coordinate location. `*From*` names a source. `*Into*` names a write
  target.
- Test arrangement may use `setup*`; cleanup may use `teardown*` when RAII is
  not enough. Test type names such as `*Fixture`, `*TestSupport`, `Fake*`,
  `Mock*`, and `Stub*` are defined in Class And File Role Names.
- Do not encode test-only access in names such as `*ForTest()`. Testability
  seam rules live in `doc/dev/testing/fixtures-and-helpers.md`.

## Semantic Vocabulary

Durable project-owned identifiers use full semantic words by default. Use
precise names such as:

- `rowIndex`
- `byteOffset`
- `xPosition`
- `dictionaryId`
- `transaction`
- `argument`
- `parameter`
- `metadata`

Avoid durable spellings such as:

- `idx`
- `pos`
- `curr`
- `prev`
- `dest`
- `dict`
- `txn`
- `tmp`
- bare `arg`
- ambiguous `meta`
- `param`/`params`

Allowed project short forms are limited to stable, obvious vocabulary:

- `id` and `ids`
- `min` and `max`
- `lhs` and `rhs`
- `config`
- tiny-local `i`, `j`, and `it`
- scoped conversion `src` and `dst`
- argument-list `args`
- storage-handle `db`
- temp-file `temp`
- coordinate record fields `x` and `y`
- chrono instant samples `tp` and `t0`..`tN` (see *Time And Duration Names*)

Use `cancelled` for Aobus domain state, user-facing domain text, and async
control-flow names. Use `canceled` only when matching external API, framework,
or protocol spelling, such as GTK-style signal names.

## Boundary Vocabulary

External API, file-format, protocol, framework, and user-interface boundaries
may keep established vocabulary when matching that boundary is clearer or
preserves compatibility. Translate those names before they become
project-owned API.

Examples:

- LMDB `txn` and `MDB_*` names inside the LMDB wrapper.
- SPA/PipeWire `dict` and `param`.
- ALSA `params`.
- MP4 `meta` atoms.
- Keyboard `Meta`.
- Existing CLI flags or serialized keys such as `--dict`, `--meta`, and
  YAML `meta:`.
- Binary-format field-layout structs such as ID3v2 `HeaderLayout` and
  MPEG `FrameLayout`.

## Review Practice

- Prefer renaming unclear project-owned names in code over documenting narrow
  exceptions.
- Apply the tie-break and no-churn meta-rules from *How To Use This Document*:
  synonym swaps within a family are not review findings; a name that misstates
  the contract, layer, or responsibility always is.
- Document a boundary principle once when a repeated external vocabulary could
  confuse future reviewers.
- Keep mechanical naming checks in `./ao name-audit`; leave semantic role
  judgments to review unless they can be enforced without exception churn.
- When a review outcome is too specific to be a principle, record it in
  *Appendix: Precedents* instead of adding a body rule, and remove it once the
  code makes it redundant.

## Appendix: Precedents

This appendix is deliberately small. It records the few concrete decisions
that are prone to regressing; it is not review history. Every entry must name
the body principle it applies and a removal condition, and is deleted when
that condition holds. A candidate entry that cannot name both does not belong
here — fix the code or add a principle instead.

- Do not reintroduce a `TuiPresentation`-style umbrella file. The TUI
  presentation module is split by concrete concept into `LibraryNavigation`,
  `TrackListEntry`, `TrackDetailLines`, `TrackPresentationNavigation`,
  `PlaybackStatusFormatter`, `QualityIndicatorStyle`, and
  `SelectionNavigation`.
  Principle: the umbrella-file ban in *TUI Roles* and *File Names*.
  Remove when: that file set no longer matches the TUI presentation layer, or
  the ban is enforced mechanically.
- `*RowHitRegion`, not `*Box`, names rendered terminal row hit-test geometry;
  the type is not a layout box with size and positioning behavior.
  Principle: name the contract, not the storage or visual shape (*Type And
  Contract Names*).
  Remove when: the type is gone or a body rule covers hit-test geometry names.
