---
id: rfc.0002.application-concept-debloat
type: rfc
status: accepted
domain: system
summary: Proposes removing duplicated concepts, one-shot binding protocols, and redundant boundary enforcement across the repository without changing the layer map or the authorities it protects.
depends-on: none
---
# RFC 0002: Application concept debloat

## Problem

The layer map in the [system architecture](../architecture/system-overview.md) is healthy.
`Core -> CoreRuntime -> AppRuntime -> UIModel -> native frontend` states one dependency direction, and each layer has a real reason to exist.
The cost is not layer count.
It is the number of public nouns inside each layer.

Three patterns produce most of that cost.

**One responsibility is spelled as many named roles.**
`app/include` currently holds 180 public headers: 75 under `ao/rt`, 97 under `ao/uimodel`, 5 under `ao/desktop`, 3 under `ao/i18n`.
Many name a role rather than an owner.
`ListPresentationPreferenceLifecycle` has exactly one member, an `async::Subscription`, and mutates a `std::map<ListId, std::string>&` borrowed from the owner that also constructs it.
`TrackFieldPresentationPolicy` is already free functions rather than a class, but publishes five independent queries where one closed switch on `rt::TrackField` decides all five answers together.
Not every small type is an instance of this: `TrackSourceLease` looks like one and is not, for reasons the survival test below works through.

**One lifetime is expressed by several coordinators, bind protocols, and nullable bags.**
`GtkUiDependencies` carries 15 fields, 10 of them nullable raw pointers, and `LayoutBuildContext` carries 11 fields including that whole bag plus `rt::AppRuntime&`.
`ResourceByteLoader` supports default construction plus three bind overloads plus `unbind()`, although desktop library switching is settled by process restart in [Decision 0009](../decision/0009-use-process-restart-for-gtk-library-switching.md) and [Decision 0005](../decision/0005-use-process-restart-for-winui-library-switching.md), so no production path rebinds one to a second runtime.
WinUI stacks `LibraryWindowSession` over `LibrarySession` over `MainWindow` over `UiCoordinator`, where `UiCoordinator` exposes three accessors and a destructor-only `retire()`.
The `AppRuntime : CoreRuntime` relationship also leaks raw storage authority into GTK: `SmartListDialog` reaches `musicLibrary()` twice to build its private preview evaluator and projection even though existing runtime owners already perform those jobs.

**One boundary is protected by types, documents, CMake regexes, and runtime checks at once.**
`app/CMakeLists.txt` is 916 lines and declares 22 `add_custom_target` boundary checks, 17 generated guardrail regex files, 23 invocations of `AssertNoForbiddenIncludes.cmake`, and 9 configure-time `message(FATAL_ERROR)` self-tests of those regexes.
The enforcement now has a maintenance cost comparable to the code it protects, and in one case it has begun to shape the code.
`rt::Library` carries `createList`, `updateList`, `deleteList`, `previewDeleteList`, `deleteListAndDescendants`, and `previewDeleteListAndDescendants` purely because `AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX` bans the identifier `LibraryWriter` and the token `.writer(` anywhere under `app/linux-gtk`.
The header says so directly: "linux-gtk is barred from reaching LibraryWriter directly (frontend-core guardrail), so these stay."

A parallel duplication exists in the shared component vocabulary.
`SharedLayoutComponentType` is simultaneously an enum, a type-string mapping, a reverse lookup, a descriptor factory, three descriptor-extension functions, and `sharedVocabularyDepartures()`, a detector written to prove that the single source of truth is in fact single.
Text has a related shape.
`MessageId` declares 881 message identifiers plus `Count` in `app/include/ao/i18n/MessageCatalog.h`, and `app/i18n/MessageIds.h` restates all 881 pairs as a 930-line hand-maintained id-to-key table that must be kept in sync by hand.
`PresentationTextCatalog` then wraps the catalog with per-feature formatting, and `GtkTextId` declares a separate `std::uint8_t` id space of 97 GTK chrome identifiers plus `Count`.
The TUI repeats the same shape with 89 `TuiTextId` identifiers plus `Count` and a parallel id-to-`MessageId` array.
These are frontend inventories rather than full copies of the canonical message enum, but they still create two more id spaces over one catalog and a translation step at every call site.

The [naming convention](../development/naming-convention.md) and [UIModel organization guide](../development/uimodel-organization.md) are not the cause, but they lower the friction.
A vocabulary with formal meanings for dozens of role suffixes makes it easy to justify a new named role and hard to justify deleting one.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0001](0001-library-mutation-savepoints.md).

RFC 0001 designs savepoints inside `library::WriteTransaction` and `LibraryMutationService::Mutation`.
This proposal renames `LibraryMutationService` to `LibraryWriteLane` and makes it an implementation type, and renames `LibraryWriter` to `LibraryCommands`.
Neither proposal blocks the other, but if both are implemented the surviving type names and the mutation-lane phase contract must be reconciled in one change rather than twice.

## Goals

- Apply one evidence-based concept audit to Core, runtime, UIModel, every frontend, the CLI, tools, and production-facing test seams rather than treating `app/include` as the whole repository.
- Reduce the number of public concepts a contributor must learn before changing one feature, measured in public declarations, dependency edges, construction hops, and rebuild fan-out rather than in file or line counts.
- Delete every two-phase construction protocol that no production path exercises, while keeping the rebinding that shell generation replacement genuinely requires.
- Replace stored or re-exported nullable collaborator bags with exact collaborator capture at registration time, while permitting exact one-shot construction arguments.
- Express component vocabulary as one canonical descriptor table instead of a framework that reconciles several restatements of the same table.
- Derive the message-id enum and its key table from one canonical machine-readable source so no hand-maintained table can drift.
- Converge boundary enforcement on one declarative architecture audit, and prefer physical visibility over source regex only where physical visibility can actually carry the rule, retargeting the rest rather than dropping it.
- Keep every authority named under Protected structure exactly as it is.
- Keep test coverage at behavior level while class-mirroring test files disappear with the classes they mirrored.

## Non-goals

- Change the top-level layer map or the composition-root model owned by the [system architecture](../architecture/system-overview.md).
- Merge `PlaybackSuccession`, `PlaybackTransport`, and `PlaybackService`, or otherwise reconstitute a playback god object.
- Merge the transaction capability split of `MusicLibrary`, `WritableMusicLibrary`, `ReadTransaction`, `WriteTransaction`, and `LibraryWrite`.
- Collapse the failure channels defined by the [failure and reporting architecture](../architecture/failure-and-reporting.md) into a shared outcome or event bus.
- Share widget abstractions across GTK, WinUI, and a future AppKit shell.
- Introduce an actor framework, workflow engine, dependency-injection container, service locator, generic state machine, or reflection-based persistence owner.
- Reduce line count as an objective in its own right.
- Change any user-visible behavior, persisted format, or command surface.

## Proposed design

### What this RFC asks reviewers to accept

Everything below carries one of three tiers, so that accepting this proposal has a definite meaning.

| Tier | Meaning | Marked as |
|---|---|---|
| Rule | An architectural constraint this proposal asks to adopt. Changing one requires amending the RFC. | **Rule.** |
| Design | A recommended shape. A better one may replace it during implementation without amending the RFC. | **Design.** |
| Candidate | An item listed to show what the diagnosis implies. It is validated or dropped on its own evidence. | **Candidate.** |

Accepting this proposal means accepting the rules, the protected structure, the non-goals, and the phase ordering with its hard constraints.
It does not mean approving any specific class merge, field list, or percentage.
Per-item sequencing, the full renaming table, and per-change rollback plans belong in the local ignored plan tree rather than here.

The rules are:

1. A public concept must answer what it owns, what it guarantees beyond the types it already holds, and which correctness contract is lost if it is deleted.
2. One-shot construction/runtime attachment and shell-generation rebinding are different phenomena; only the one-shot form is converted to constructor binding.
3. A stored or re-exported dependency aggregate carries build-traversal state only; session-lifetime collaborators leave it. An exact argument object consumed once by a constructor or factory is not an ambient dependency bag.
4. Component vocabulary is data with one canonical source, not a framework reconciling several restatements.
5. A shared utility is not extracted without field-level semantic identity; the default answer is no.
6. A boundary rule is retargeted or promoted, never deleted merely because the code it constrained moved.
7. Renames land with the consolidation that touches the file; standalone rename changesets are forbidden.
8. Success is measured in public declarations, dependency edges, construction hops, and rebuild fan-out. Header count is a secondary signal only.

Rule 3 distinguishes an argument object's lifetime and queryability, not whether every argument is mandatory.
An exact one-shot constructor or factory object may contain an explicitly nullable collaborator when absence is part of that operation's semantics, provided the callee immediately consumes the object into its own fields and never stores or re-exports the bag.
`ActivityStatusWidgetDependencies::libraryTasks`, whose absence means that no task feed is presented, is permitted by that rule.

### Concept survival test

Every public class, public header, and separate build target answers five questions.

1. Does it own independent mutable state, a resource, or a lifetime?
2. Does it make an illegal state unrepresentable in the type system?
3. Does it isolate a real platform, ABI, heavy-dependency, or transaction boundary?
4. Do at least three independent production consumers use it with identical semantics?
5. Do multiple real implementations exist, requiring runtime injection or polymorphism?

These are review prompts, not a scoring rubric, and this proposal does not propose to score anything.
Entries below already override a single question on evidence: `LibraryStamp` fails question 4 and survives, `ShellGenerationSequence` has one consumer and survives on question 2.
A rule that needs exceptions before it lands is a heuristic.
**Rule.** What generalizes is the shorter form used at review time: what does it own, what does it guarantee beyond the types it already holds, and which correctness contract is lost if it is deleted.

Worked examples against current code:

| Type | Evidence | Disposition |
|---|---|---|
| `LibraryMutationService` | Owns lane lifetime, transaction admission, publication settlement, and closing | Keep, rename, make private |
| `async::RequestCoalescer` | Owns flight identity and independent cancellation, several production consumers | Keep |
| `uimodel::OutputDeviceIntent` | Not default-constructible, so a dependency bundle cannot omit a recorder silently | Keep, unrenamed |
| `rt::TrackSourceLease` | Every signature taking one states a non-null source; roughly ten production consumers | Keep |
| `async::Sleeper` | Separates runtime scheduling from time; its controlled implementation makes cancellation, expiry, and restart-deadline behavior deterministic in tests | Keep |
| `rt::AppRuntimeDependencies` | One-shot factory arguments, including transferred ownership and explicit optional test seams; not stored or re-exported | Keep; consider an `Options` rename only with structural work |
| `gtk::ActivityStatusWidgetDependencies` | Exact constructor inputs consumed into the widget's own fields; nullable `libraryTasks` models the optional task feed, and no caller can query the object as a service catalog | Keep; reassess only if structural work makes the aggregate unnecessary |
| `uimodel::ListPresentationPreferenceLifecycle` | Holds one subscription on behalf of another owner | Absorb into that owner |
| `winui::UiCoordinator` | Groups four members and performs bind/retire | Absorb into the session, retirement protocol intact |

`TrackSourceLease` is the instructive one, because an earlier draft of this analysis proposed deleting it as a `std::shared_ptr<TrackSource>` wrapper whose only protocol is one `AO_EXPECTS`.
That mistook the implementation of an invariant for the absence of one.
The third review question answers itself here: deleting it loses the non-null guarantee at every interface that currently states it, including `TrackSourceCache::acquire`, `sourceError`, `buildImplementation`, `TrackListProjection`, `ListOrderSource`, `SmartListSource`, and `PlaybackCursorSession`.
Failure already travels on `Result<TrackSourceLease>`, so a null pointer is not a failure channel; substituting a raw `shared_ptr` would readmit null into roughly ten signatures that no longer have to check for it.
A wrapper over one STL type must prove an additional invariant, and this one does.

The test replaces the implicit rule that a name conforming to the naming convention is thereby a justified abstraction.

### Repository-wide survey

The survival test applies to all production C++ and to any production header kept alive only by tests.
The baseline inventory is deliberately broader than the 180 application public headers.

| Area | Headers | `.cpp` files | Interpretation |
|---|---:|---:|---|
| Core public API, `include/` | 133 | 0 | Public capability and format surface; low consumer count alone is not grounds for deletion. |
| Core implementation, `lib/` | 116 | 177 | Private platform, codec, store, and algorithm boundaries; optimize for dependency isolation. |
| Application and frontends, `app/` | 481 | 459 | Includes the 180 shared application headers plus private GTK, WinUI, TUI, and CLI modules; the main debloat surface. |
| Tools, `tool/` | 41 | 44 | External plugin/API shapes may require one concrete type per registered implementation. |
| Tests, `test/` | 80 | 665 | Fixture and protocol headers are not production concepts and do not count toward product targets. |

These counts are an inventory, not a quota.
Phase 0 records them with `rg --files <area>` filtered separately for `.h` and `.cpp`, so later changes compare the same roots and extensions.
They exclude build scripts, generated files, assets, and documents, which are reviewed by dependency edge and authority rather than by extension count.

The non-C++ roots are in scope under that second test.
`cmake/` and the top-level build manifests are judged by how many independent traversals and rule declarations they require; `script/ao/` is judged by user-visible command capability and shared execution state; `asset/`, `config/`, dependency manifests, and `doc/` are judged by whether one fact has one authoritative source.
They receive no file-count reduction target.

The full-repository pass produced both deletion candidates and important counterexamples.

| Area | Evidence | Disposition |
|---|---|---|
| Core test-only API | `StrongTypeStream.h` has one consumer, `StrongTypeTest.cpp`, while the formatter adapter has many production consumers. The stream test exists only to test otherwise-unused production API. | **Design.** Delete the stream adapter and its structural test in Phase 1a. |
| Enum reflection surface | `EnumName.h` is included only by YAML reflection, `enumFromName()` has no caller, and the only `ReflectEnumTraits` specialization is a test fixture. | **Candidate.** Delete `enumFromName()`; either keep the minimum enum-writing shape in the YAML reflection module when a production use is established, or delete the test-only reflection feature until one exists. |
| Shared utility placement | `StringArena` owns a real stable-view and allocation-lifetime contract, but its only production client is `TrackListProjection`; its other clients are its unit test and the performance review. | **Candidate.** Keep the type and its performance assertions, but move it intact to the projection implementation boundary rather than presenting one application optimization as shared Core API. |
| Core format visibility | Public `media/ogg/PageLayout.h` is consumed in production only by `Demuxer.cpp`; the other consumers are Ogg/Opus tests that construct or corrupt pages. Its packed layout and constants are real, but they are parser implementation vocabulary rather than an application-facing capability. | **Candidate.** Move the header unchanged to the private Ogg implementation boundary and let the focused format tests use that private seam. Do not fold the packed layout into the decoder or duplicate it in fixtures. |
| Core diagnostics | `TrackViewRawAccess` has one production consumer, the CLI raw dump, but deliberately bypasses decoded-accessor validity so corrupt bytes remain inspectable. `query::detail::OperatorTable` has two consumers but is the checked single source of truth for operator spelling and class. | Keep both. They demonstrate why consumer count is evidence, not a verdict. |
| Core audio and storage | Decoder sessions, backend providers, store readers/writers, LMDB transactions, and `audio::Subscription` isolate real codec, platform, transaction, and dependency-direction boundaries. | Keep in substance. Private closed `*Policy` helpers may be absorbed when their owner changes, but receive no deletion quota. |
| Runtime and UIModel | One-shot initialization remains in `ActivityStatusFeedProjection`, `ResourceByteLoader`, WinUI `TrackListController`, and `SmtcBridge`; pure rule fragments remain spread across field editing, field-grid presentation, list actions, list presentation, filtering, and output selection. | Constructor-close the one-shot states and consolidate the rule fragments by feature, as detailed below. |
| Runtime diagnostics | Four public `*OperationCounts` values and their accessors, `resourceCarrierIndexBuildCount()`, `ShellGenerationSequence::stagedCount()`, and both public cache `cachedCount()` accessors have no production reader; tests use them to prove incremental update, generation, and cache behavior. Source-private `resourceCount()` and `retiredCount()` probes already have the right visibility. | **Candidate.** Preserve deterministic assertions through source-private test probes or equivalent instrumentation, then delete the production-facing diagnostic declarations. Do not replace them with timing-sensitive tests. |
| Runtime dead surface | `PlaybackCursorSession::consecutiveFailureCount()` has no reader in production or tests. | **Design.** Delete the accessor in Phase 1a; the private state and behavior it reports are unchanged. |
| GTK, WinUI, and TUI | Several policy or bootstrap headers have one production owner; GTK has ten family/per-component registration headers where WinUI has one private registration seam; GTK and TUI duplicate the same three-line platform-audio bootstrap that WinUI writes inline. | **Candidate cohort.** Absorb single-owner rules, consolidate declaration-only registration seams, and inline repeated composition-root plumbing. |
| CLI | Six command headers primarily expose one `configure*Command` declaration, while the command implementations are 19 to 1,548 lines and benefit from separate compilation. | **Candidate.** Keep the `.cpp` units and consolidate only registration declarations; preserve feature-specific test seams such as list-order validation. |
| Lint tool | 38 concrete checks are passed as types to Clang-Tidy's `registerCheck<T>()` plugin API and compiled as independent implementation units. | Keep. This is real external polymorphism and registration, not a role-per-function application design. |
| Tests | Most headers are fixtures, fake backends, fatal-probe protocols, or cross-file test support. | Exclude from public-concept percentages; move behavior tests with surviving owners and remove tests that only mirror deleted structure. |
| Build and portal | Repeated CMake source scans restate traversal mechanics, while the `script/ao/command` modules each own a user-visible `./ao` verb and already share one command registry. | Consolidate the CMake audit declarations as proposed below. Keep command modules and the specialized document, dependency, test, and lint engines separate; line count is not evidence that they are one concept. |
| Assets, configuration, and documents | Brand variants, placeholder artwork, dependency manifests, persisted schemas, and current documents are authoritative data or licensed artifacts rather than application roles. Documentation does contain architecture pages whose independent ownership may disappear after code consolidation. | Keep data and licensed variants. Remove only duplicated authored inventories, and adjudicate documentation merges last against the post-refactor ownership graph. |

**Rule.** A repo-wide audit may expand where the heuristic is applied, but it may not turn the heuristic into a repo-wide percentage reduction.
Core formats, native platform seams, plugin contracts, and tests optimize for different dependency graphs than shared application UI concepts.

### Protected structure

These are not debloat targets and must survive unchanged in substance.

| Structure | Why it stays |
|---|---|
| Library mutation lane | Owns FIFO command admission, `Open`/`Closing`/`Closed`, the `PreTransaction`/`InTransaction`/`AwaitingPublication` phases, the rule that a native transaction never spans `co_await`, non-cancellable post-commit settlement, and the `Busy`/`Stale`/`Unavailable` distinction. See the [runtime mutation specification](../spec/library/runtime/mutation.md) and [Decision 0015](../decision/0015-sequence-live-runtime-library-writes.md). |
| Core transaction capability | The `MusicLibrary` / `WritableMusicLibrary` / `ReadTransaction` / `WriteTransaction` / `LibraryWrite` split protects real transaction lifetime and write authority. Header coupling may shrink; semantics may not merge. |
| Playback authority split | `PlaybackSuccession`, `PlaybackTransport`, and `PlaybackService` own source succession, the transport and audio bridge, and application commit and publication respectively. See the [playback architecture](../architecture/playback.md). |
| Failure channels | `Result`, typed asynchronous failure, cancellation, the fatal contract, notification, and logging stay distinct. |
| Native frontend strategy | Shells share semantic state and layout vocabulary, never widget abstractions. See the [application shell architecture](../architecture/application-shell.md). |
| Authoring evidence | `BoundTrackTargets` and `BoundListOrder` keep runtime-owned construction and revalidation at commit. Only their shared field pair is factored. |
| Source identity and lifetime | `TrackSourceLease` keeps shared ownership and non-null source identity in one capability value; sources, caches, projections, and playback cursors continue to state that contract in their signatures. |
| Codec, backend, and store boundaries | Decoder sessions and backend providers have multiple real implementations; store readers/writers and LMDB transactions carry scoped authority over persisted formats. File consolidation may not erase these capabilities. |
| External tool contracts | Clang-Tidy check types and their registration units keep the shape required by the plugin API. Application naming preferences do not override an external ABI or template-registration contract. |

Within the mutation lane this proposal performs private naming cleanup only.

### Runtime composition

`CoreRuntime` currently has a virtual destructor, a virtual `shutdown()`, a protected constructor, and a protected `initialize()`.
`AppRuntime` is `final`, is the only derived class, and calls the base `shutdown()` after its own.
There is no substitutable polymorphism; the relationship is ownership.

```cpp
class CoreRuntime final
{
public:
  static Result<std::unique_ptr<CoreRuntime>> create(/* ... */);
  void shutdown() noexcept;
};

class AppRuntime final
{
public:
  static Result<std::unique_ptr<AppRuntime>> create(AppRuntimeDependencies dependencies);
  void shutdown() noexcept;

  // Core services stay reachable by name, without IS-A and without handing out the core owner.
  Library const& library() const noexcept;
  Library& library() noexcept;
  async::Runtime& async() noexcept;
  TrackSourceCache& sources() noexcept;
  NotificationService& notifications() noexcept;
  CompletionService& completion() noexcept;
  TextOrderingPolicy const* textOrderingPolicy() const noexcept;
  std::filesystem::path const& musicRoot() const noexcept;

  PlaybackService& playback() noexcept;
  WorkspaceService& workspace() noexcept;
  ViewService& views() noexcept;

private:
  std::unique_ptr<CoreRuntime> _corePtr;
};
```

`AppRuntime` forwards the core service accessors explicitly.
This is required, not optional.
Frontends currently make 126 calls through the five application-facing core services: `library()` 52, `async()` 36, `notifications()` 27, `completion()` 8, and `sources()` 3.
They make another 6 calls to `textOrderingPolicy()` and 7 calls directly through `AppRuntime` to `musicRoot()`.
Those 139 call sites retain their source shape through the complete explicit forwarding face shown above.
Without a forwarding face, removing inheritance would force a runtime-wide collaborator rewrite in the same change, which is the fan-out this proposal is trying to avoid.

The three prohibited outcomes are named explicitly.
There is no `appRuntime.core()` returning the `CoreRuntime` owner or reference; only the composition root holds the `std::unique_ptr<CoreRuntime>`.
There is no implicit derived-to-base conversion, which is what `ResourceByteLoader` relies on today and what WinUI currently explains with a comment at the call site.
A leaf that needs one service takes that one service, and converting leaves from `AppRuntime&` to exact collaborators proceeds incrementally after the composition change, not inside it.

`musicLibrary()` is deliberately not forwarded.
GTK currently reaches it twice in `SmartListDialog`: once to construct a private `SmartListEvaluator`, and once to construct the preview `TrackListProjection`.
Phase 5 removes both calls in the composition change.
GTK aligns its expression preview with WinUI by using `TrackSourceCache::acquire(SourceSpec)`, and transient projection construction moves behind `ViewService`, which already owns the `MusicLibrary` dependency.
The frontend receives the source lease and projection it needs, not a new raw-storage interface.

`databasePath()` is also deliberately not forwarded.
The current frontend audit has no call through `AppRuntime`; the four frontend `databasePath()` spellings are calls on `LibraryPaths`, while the CLI inspection path uses `CoreRuntime` directly.
The [system architecture](../architecture/system-overview.md) continues to own that CLI inspection, dump, verification, relink, and interchange hatch.
Composition therefore makes any new frontend use of either raw-storage accessor fail to compile.

This deletes the virtual lifecycle, deletes the protected half-constructed state, and makes shutdown order a property of one composition owner.

### Library application surface

`rt::Library` is the CQRS facade.
Its role names describe mechanics rather than what a caller holds.
`LibraryReader` owns one coherent read transaction for its lifetime, which is a snapshot.
`LibraryWriter` exposes no transaction scope at all and is an asynchronous semantic command surface.

```cpp
class Library
{
public:
  LibrarySnapshot snapshot() const;
  LibraryCommands& commands() noexcept;
  LibraryJobs& jobs() noexcept;
  LibraryChanges const& changes() const noexcept;
};
```

The six list-mutation forwarding methods are deleted, and the frontend path becomes `frontend -> UIModel session -> LibraryCommands`.
That path already exists for two thirds of authoring: `TrackAuthoringSession`, `ListOrderAuthoringSession`, and `ListMembershipAuthoringSession` in `app/uimodel/library/` use `LibraryWriter` directly, and the UIModel guardrail regex does not ban it.
Only list creation, update, and deletion lack a UIModel owner.

This means the forwarding removal is blocked on a new UIModel `ListAuthoring` free-function module, not on the runtime work, and it must land in the same change that retargets `AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX`.
Deleting the forwarders while that regex still bans `.writer(` under `app/linux-gtk` would break the GTK build.

`LibraryTaskService` becomes `LibraryJobs` and narrows to scan, import, export, and identity backfill with their progress feed.
`loadResourceAsync` is not republished as a `LibraryResourceService`; resource materialization stays in the runtime implementation, `ResourceByteLoader` receives a narrow `ReadBytes` callable at construction, and `CoreRuntime` binds that callable to the internal materializer.
`resourceCarrierIndexBuildCount()` moves to a private test seam.
This preserves every contract in the [cover-art delivery specification](../spec/resource/cover-art-delivery.md) and the [library task-execution specification](../spec/library/runtime/task-execution.md) while removing one public role.

### Shared evidence and shared utility

`LibraryAuthoringAvailability`, `BoundTrackTargets`, and `BoundListOrder` each carry `runtimeInstanceId` and `libraryRevision`, and the two evidence classes reimplement the same three-clause `matches()`.

```cpp
struct LibraryStamp final
{
  std::uint64_t runtimeId = 0;
  std::uint64_t revision = 0;

  bool matches(LibraryAuthoringAvailability const& availability) const noexcept;
};
```

This extraction is proposed on an explicit exception to question 4 of the survival test.
There are two consumers today, roughly six duplicated lines, and both live in `app/include/ao/rt/library/LibraryAuthoring.h` forty lines apart.
The justification is not deduplication; it is that a third binding kind is likely and must not copy `matches()` a third time.
No other extraction in this proposal is granted the same exception.

Two further extractions are proposed as **not extracted by default**.

`LruMap<Key, Value, Weight>` over `ResourceByteCache`, `IndexedTrackRowCache`, and the GTK `ImageCache` is rejected unless a field-by-field comparison first shows identical eviction semantics, and unless the result deletes three copies without introducing a virtual eviction policy, a policy-template forest, a generic loader, or a callback framework.
A shared `GenerationGate` is rejected unless the shell generation, resource generation, and other callback gates have identical settle and retire semantics.

Three fields spelled `generation`, or three containers described as caches, are not evidence.
The default answer is no, and the burden is on the extraction.
Repeating a dozen lines of algorithm is cheaper than getting one lifetime wrong, and this is a constraint of the proposal rather than a question left open for the implementer.

### Layout and shell

This area carries the largest concentration of public nouns and the largest expected reduction.

**The shared component vocabulary becomes data.**
`SharedLayoutComponentType`, `componentTypeName()`, `sharedComponentFor()`, `sharedComponentDescriptor()`, `withShellProperties()`, `withShellLayoutProperties()`, `withShellActionSlots()`, and `sharedVocabularyDepartures()` are replaced by one canonical schema table.

*Design.* The shape, not the field list, is what review is asked to accept:

```cpp
struct ComponentSchema final
{
  std::string_view id;
  std::string_view displayName;
  LayoutComponentCategory category;
  LayoutSurfaceCapabilityMask surfaces;
  std::size_t minChildren;
  std::optional<std::size_t> optMaxChildren;
  bool persistentState;
  std::span<PropertySchema const> properties;
  std::span<PropertySchema const> layoutProperties;
  ActionSlotMask actionSlots;
  std::span<DefaultActionBinding const> defaultActions;
};

std::span<ComponentSchema const> sharedComponentSchemas();
```

`displayName` and `category` carry over from today's `LayoutComponentDescriptor` rather than being dropped.
The action fields are spelled as the plain values that replace `LayoutComponentActionPolicy`, not as that type, since it is one of the eight collapsing below.

A shell imports the entries it presents and adds its own descriptors.
A table that is genuinely the single source of truth does not need a detector proving that it is, so `sharedVocabularyDepartures()` disappears with the restatements it existed to reconcile.
**Rule.** The direction is schema to document, not document to compiler.
The schema table is the source; the [component vocabulary reference](../reference/shell/component-vocabulary.md) is generated from it and remains the reader-facing authority for the exact inventory.
Treating a Markdown table as compiler input would make the build depend on prose formatting, which is the fragility this proposal is trying to remove rather than relocate.

**Statefulness becomes a descriptor field.**
`StatefulLayoutComponentType` currently decides statefulness by comparing against two type strings, one of which it documents as GTK-only while declaring it in shared UIModel.
`LayoutComponentDescriptor` already carries `surfaces` and `actionPolicy`; adding `persistentState` removes the predicate and the layering leak together.
This change does not depend on the full schema-table rewrite and can land on its own.

**Action micro-types collapse into two concepts.**
`LayoutActionDescriptor`, `LayoutActionCatalog`, `LayoutActionCapabilities`, `LayoutActionSlot`, `LayoutActionBinding`, `LayoutActionSlotResolution`, `LayoutActionValidator`, and `LayoutComponentActionPolicy` become `LayoutSchema`, which states what an authored document may use, and a frontend-local `ActionRegistry`, which holds executable callbacks.
Descriptors, slots, properties, and validation are values and functions inside the schema module, not eight public roles.

**The layout state stack collapses.**
`ShellLayoutSessionModel`, `LayoutRuntimeState`, `LayoutBuildStateView`, and `StatefulComponentState` are four types describing one lifetime.
`LayoutBuildStateView` in particular is two types in one: every accessor branches on `_runtimeState == nullptr` to choose between a borrowed carrier and an explicit candidate.

```cpp
class LayoutSession final
{
public:
  LayoutBuildSnapshot buildSnapshot(LayoutSurface surface) const;
  ComponentStateBinding stateFor(LayoutNode const& node);

  void apply(LayoutDocument document);
  void resetComponentState();
  std::optional<PanelSizePromotion> preparePanelSizePromotion(
    LayoutComponentStateDocument const& runtimeComponentState) const;
};
```

What survives is one mutable `LayoutSession`, one immutable `LayoutBuildSnapshot`, and one `ComponentStateBinding` that carries a real generation fence.
The behavior owned by the [layout lifecycle specification](../spec/shell/layout-lifecycle.md) and the state format owned by the [layout state reference](../reference/shell/layout-state.md) are unchanged.

**`ShellGenerationSequence` stays shared.**
The earlier draft of this analysis proposed demoting it to a WinUI-private nested type because WinUI is its only consumer.
That is the wrong direction.
It answers question 2 directly: a callback that outlives its generation finds a closed gate, which is an illegal state made unrepresentable.
GTK's `LayoutHost::PreparedTree` carries only a bare `std::uint64_t componentStateGeneration` with no gate, so GTK lacks this capability rather than not needing it.
The type keeps its public home in UIModel and its current name.
Dropping the `Shell` prefix was proposed in an earlier draft and is withdrawn: it is a standalone rename with no structural gain, which rule 7 forbids.
Phase 4 may either give GTK the same gate or retain GTK's destroy-and-recreate model after proving callbacks cannot outlive the rebuilt tree; neither choice changes the shared type's survival.

### Frontend sessions and construction

**Separate rebinding from one-shot attachment, then delete only the one-shot protocols.**

`bind`/`unbind` and `initialize` name three unrelated phenomena in this codebase, and conflating them would break mechanisms this proposal is otherwise protecting.

| Phenomenon | Example | Rule |
|---|---|---|
| Required initial state supplied after construction | `ActivityStatusFeedProjection::initialize()` | Take the initial feed in the constructor. There is no valid empty projection phase and no second initialization. |
| Session-level runtime attachment with no replacement | `ResourceByteLoader` with three bind overloads, `winui::TrackListController`, `winui::SmtcBridge` | Bind in the constructor; end through explicit retirement or destruction. No production path replaces their runtime, because desktop library switching restarts the process. |
| Shell generation replacement | `winui::VolumeControl`, `SeekControl`, `TransportButton`, `PlaybackTimeControl`, `OutputDeviceControl`, `ActivityStatusControl`, `SoulTransportButton` | Rebinding is legitimate. Keep `bind`/`unbind`, or destroy and rebuild behind a generation gate. |

The third row is not a leftover.
Those controls carry the comment "Blank the widget between bindings. Only a rebind has anything to show," they are shell-persistent members held across layout rebuilds by `MainWindowPlayback`, and `ShellGenerationSequence` exists precisely because callbacks outlive a generation.
A blanket rule saying no type qualifies today would authorize deleting these, and would then collide with the generation gate this proposal explicitly preserves.

The first two rows change; shell-generation replacement in the third row survives:

```cpp
ActivityStatusFeedProjection(MessageCatalog catalog,
                             NotificationFeedState const& initialFeed);
ResourceByteLoader(CoreRuntime& runtime);
TrackListController(AppRuntime& runtime, TrackColumnLayouts& layouts, MessageCatalog catalog);
SmtcBridge(HWND window,
           DispatcherQueue dispatcher,
           AppRuntime& runtime,
           PlaybackActions& actions,
           ResourceByteLoader& resourceBytes);
```

`ActivityStatusFeedProjection` is the narrow Phase 1c signature-cleanup exception: `initialize()` only projects the supplied initial feed into view state and establishes no subscription, resource, or teardown obligation.
Moving that argument into the constructor has no lifetime effect and does not begin the Phase 2 session work.

`ResourceByteLoader(CoreRuntime&)` is the Phase 2 form and uses today's existing constructor at the sole production bind site.
Only in Phase 5, as `AppRuntime` stops deriving from `CoreRuntime` and resource loading moves out of `LibraryJobs`, does that constructor narrow to `ResourceByteLoader(async::Runtime&, ReadBytes)`.
The remaining sketch uses end-state feature-capsule names.
Phase 2 constructor-binds today's `TrackColumnLayoutState` and `PlaybackCommandSurface`; the later feature-capsule changes replace those names with `TrackColumnLayouts` and `PlaybackActions` rather than pulling unrelated renames into the lifetime change.

Staged construction stays available to a composition root through `std::unique_ptr` or `std::optional`.
It stops being a half-initialized state machine replicated in every leaf that only ever binds once.
`SmtcBridge` is constructed at today's sole `bind()` site after native activation and is reset before the resource loader; its control disablement, event revocation, and artwork cancellation remain explicit retirement obligations.

Staged lifecycle is not banned.
`GtkStyleRuntime::initialize()` remains idempotent process activation paired with `shutdown()`, playback-session persistence starts only when desktop startup admits persistence, and MPRIS starts only after its D-Bus environment exists.
Those transitions carry real temporal meaning rather than filling a constructor ordering gap.

**Delete the dependency bags.**
`GtkUiDependencies` is deleted rather than modernized; a `ServiceProvider` would be the same aggregate with a newer name.
Component factories capture their exact collaborators at registration.

```cpp
registry.add(
  "playback.seekSlider",
  [&positionModel, &clock](LayoutNode const& node, BuildState const& state)
  { return buildSeekSlider(node, state, positionModel, clock); });
```

The rule for what remains is stated rather than sketched, because the field list is what the implementation decides and the rule is what review is being asked to accept:

> A build context carries only state scoped to one build traversal. Every session-lifetime collaborator leaves the aggregate.

Applied to today's 11-field `LayoutBuildContext`, that keeps the traversal state and removes the collaborators.

| Field | Disposition |
|---|---|
| `surface`, `parentWindow`, `registry`, `actionRegistry` | Build environment; stays |
| `detailScope`, `detailUndo` | Push/pop state mutated during the build recursion; stays |
| `timeoutScheduler` | Scheduling for one traversal; stays |
| `buildState` | Stays, but as a single `LayoutBuildSnapshot` instead of the current dual-mode view |
| `runtimeState` | Replaced by the borrowed `LayoutSession` |
| `runtime`, `dependencies` | Leaves; factories capture their exact collaborators |

Shrinking the context to three fields would hide the traversal state somewhere else rather than delete it, and a later change would reintroduce the aggregate under a new name.
What is being deleted is the collaborator bag, not the traversal scope.
Phase 4 may pass `TrackDetailScope` and its undo state explicitly through the build recursion instead of retaining them as context fields; either shape conforms if the state remains traversal-scoped rather than becoming another ambient object.

`OutputDeviceIntent` keeps its name and its deleted default constructor.
An earlier draft proposed renaming it `OutputSelectionRecorder`; that is withdrawn, because `discarded()` is not a recorder and the type models an intent of which recording is one case.
Deleting the dependency bundle removes half of its stated justification, and the remaining half, that every surface must state its choice, still holds.

**Collapse the owner layers, keep the retirement protocol.** *Design.*

GTK: `MainWindowCoordinator` becomes `GtkWindowSession` if it genuinely owns window-session lifetime, or is absorbed into `MainWindow` if `MainWindow` is already the natural owner.
WinUI: `LibraryWindowSession`, `LibrarySession`, and `UiCoordinator` are candidates for one `WinUiSession`.

**Rule.** Member declaration order is a fallback, not the teardown mechanism, and no owner layer may be removed by replacing an explicit retirement action with destruction order.
An earlier draft of this analysis claimed reverse destruction order was sufficient here.
It is not.
`LibraryWindowSession::retireWindow()` moves the window out of its holder, revokes `_windowClosedRevoker`, calls the implementation's `retire()`, calls `Window::Close()`, and only then releases the session; `handleClosed()` runs the same retirement from the opposite direction when the user closes the window.
WinRT and XAML objects form a reference-counted graph with projection handles that outlive C++ members, so destruction order cannot revoke an event, cannot run `retire()`, and cannot close a window.

What may be deleted is the owner hierarchy: three types expressing one session.
What must survive verbatim is the terminal sequence, its idempotence, the partial-retirement error state that `prepareLibraryRestart()` reports, and the fact that closure can be initiated from either side.
Any merged session states that protocol explicitly and keeps its own tests.
The teardown contract in the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) is unchanged, and this proposal reduces who owns it rather than how it runs.

### UIModel feature capsules

UIModel granularity, not direction, is the problem.
`Policy`, `Resolver`, `Recommender`, `Formatter`, `Store`, and `Lifecycle` frequently name pure functions or a single subscription belonging to one feature.

The rule: one user-perceivable feature keeps one stateful public owner, a few public values, and free functions.
Implementation helpers live in the `.cpp` or a private header.

A capsule is a module, not a class.
Merging every session in a feature domain into one object would replace one failure mode, a class per pure function, with a worse one, a god object per feature domain.
`TrackAuthoringSession` and `ListMembershipAuthoringSession` are therefore **not** merged: they share `BoundTrackTargets` but not a lifetime.
The first owns a draft-editing conversation with `isCurrent()` and `onInvalidated()`; the second has no invalidation observation and is a short command scope.
Question 1 of the survival test separates them, and both keep their own type in the `TrackAuthoring` module's narrow session partition.

| Consolidation | Effect |
|---|---|
| `ListMembershipEditResult::notificationText` -> presentation function | The result stops carrying localized text, and `ListMembershipAuthoringSession::begin` stops taking a `PresentationTextCatalog`. This is independent of any merge. |
| `ListOrderPolicy` + `ListOrderAuthoringSession` -> `ListOrder` module | Capability description, drag selection, and gap anchoring are free functions beside the session, not a `Policy` concept. This module shape is the model for the others. |
| `TrackFilterResolver` + `TrackFilterCompleter` + `TrackFilterViewModel` -> `TrackFilter` module | `resolveTrackFilter()` is a pure function; one module header replaces three public roles. |
| `TrackFieldEditCodec` + `TrackFieldEditPolicy` -> `TrackAuthoring` module | Parsing, writable-field classification, patch application, and mixed-value protection join the existing authoring sessions as free functions and values rather than creating another role. |
| `TrackFieldGridSchema` + `TrackFieldGridPolicy` -> `TrackFieldGrid` module | GTK and WinUI share one schema and the same visibility decisions; five `should*` functions do not require a `Policy` role or a second public header. |
| `ListActionPolicy` -> `ListActions` module | The public value and `describeListActions()` stay; the strategy-shaped suffix and standalone role disappear. |
| `SmartListTrackPresentationResolver` -> `SmartListEditing` module | Its two functions serve only the smart-list editing flow and delegate the actual recommendation to list presentation. |
| Five seek and position types -> `PlaybackPosition` module | `PlaybackClockChangeFilter` becomes private, the interpolator becomes a module helper, `formatPlaybackTime()` stays a free function, and `SeekInteraction` survives only because the pointer gesture is genuinely shared across toolkits. |
| `TrackColumnLayoutStore` + `TrackColumnLayoutState` -> `TrackColumnLayouts` | One live owner for both shells. The YAML schema serializes its snapshot. `Store` is wrong today: this is in-memory state, not a persistence location. |
| `ListPresentationPreferenceStore` + `ListPresentationPreferenceLifecycle` -> `ListPresentations` | Removes the borrowed-map aliasing between owner and lifecycle. |
| `TrackPresentationRecommender` -> `ListPresentations` | Recommendation is the fallback rule of the same list-to-presentation state, not an independently injectable role. |
| `TrackColumnLayoutPolicy` -> `TrackColumnLayouts` | Stored-order merge functions operate on the live owner's values and remain shared by GTK and WinUI. |
| Five `TrackFieldPresentationPolicy` queries -> `trackColumnDefaults(TrackField)` | One decision, one switch, one returned `TrackColumnDefaults`. |
| `OutputDeviceSelectionPolicy` -> `OutputSelection` module | Three frontends share the restore decision, so the behavior stays public; two closed functions do not become a strategy object. |
| `TrackSelectionRegionPolicy` -> GTK track-selection-region implementation | Its only production consumer is the GTK component. The UIModel public header exists to expose a one-frontend test seam and moves back to that feature. |
| `LibraryScanWorkflowResult`, `...Failure`, `...PlanDisposition`, `...Stage` -> private | Only `LibraryScanOutcome` stays public behind `runLibraryScan(...)`. Cancellation still propagates `OperationCancelled`. |

Each consolidation is one change whose success criterion is fewer public concepts, fewer public headers, and fewer consumer includes, with behavior tests retained.
The behavior owned by the [presentation specifications](../spec/presentation/track-filter.md) does not change.

### Frontend-private and command registration surfaces

The repo-wide pass also found private concepts that never reach `app/include` but still add navigation and construction cost.
The following are **candidates**, not rule-level acceptance obligations; each lands only if its local behavior tests can follow the surviving owner without adding a heavier public include.

| Cohort | Consolidation |
|---|---|
| Platform audio bootstrap | Delete the identical GTK and TUI `AudioBackendBootstrap` header/source pairs and write the three-line `createPlatformBackendProviders()` loop in each composition root, as WinUI already does. Keep the backend factory test; delete frontend tests that only prove the wrapper calls it. Do not auto-install native hardware providers in `AppRuntime::create()`, because tests and non-desktop composition roots inject providers deliberately. |
| GTK window input | Absorb `MouseNavigationPolicy` and `PlaybackShortcutPolicy` into one private window-input module or `MainWindow`; both have one production owner. Preserve the pointer-button and text-focus behavior tests. |
| GTK import/export | Move `ImportExportCoordinatorPolicy`'s chooser-cancellation and export-index decisions into the coordinator implementation. They are not an injectable policy. |
| GTK component registration | Replace five family `*Registry.h` files and five `*ComponentRegistrations.h` files with one private registration seam, or at most one aggregate and one leaf-declaration header if measured include weight favors the split. Keep per-component `.cpp` units; this is declaration consolidation, not a unity build. |
| WinUI window and shell rules | Absorb the one-consumer `WindowInteractionPolicy` into `MainWindow`. Keep `ShellState` and its enums, but replace the static-only `ShellStatePolicy` class with `classifyShellWidth()` and `resolveShellState()` in the shell-state module. |
| TUI leaf helpers | Absorb `QualityIndicatorStyle` into `QualityPanel`, and absorb the one-consumer `PlaybackActions` and `ListNavigation` helpers into the event-controller input/action module. Keep `SelectionNavigation`, `PlaybackStatusFormatter`, `LibraryNavigation`, and `TrackPresentationNavigation`, which have multiple production consumers or publish shared value types. |
| CLI command registration | Replace the six registration-only command headers with one private `CommandRegistrations.h`. Keep command implementations separate and keep `runScan()` and `validateListOrderCommandStatus()` only where production or behavioral tests need those narrower seams. |

`ImageRenderPolicy` is deliberately not in the absorption list.
Its `RenderTarget` is shared by the image widget, controller, and loader without including GTK widget types; merging it into `ImageWidgetLayout.h` would increase transitive native dependencies.
It may become `ImageGeometry` only as part of moving the geometry implementation out of `ImageWidget.cpp`, not as a standalone rename.

### Role vocabulary

The formal suffix vocabulary shrinks to the roles that carry a contract.

| Suffix | Permitted only for |
|---|---|
| `Service` | A long-lived application authority owning state, commands, and events. |
| `Session` | A temporal owner with a defined start, end, invalidation, or binding evidence. |
| `Store` | A real read/write persistence location. |
| `Cache` | Memoized state with a capacity and an eviction policy. |
| `Model` | UI-owned mutable state or an interaction conversation. |
| `Catalog` | An immutable or indexed vocabulary. |
| `Adapter` | A conversion between two explicit API shapes. |
| `Lease` | A scoped lifetime or authority token that makes release or non-null ownership explicit. |
| `Provider` | A replaceable capability with multiple real platform or codec implementations. |
| `Registry` | A live identity-to-instance/factory set, or a registration shape imposed by an external plugin API. A header that only forwards several `register*()` calls is not a registry. |
| `Reader`, `Writer` | Scoped read or write capability over a real transaction, persisted store, or byte format. |
| `Projection` | Derived state with an explicit source, refresh, invalidation, or subscription contract. |
| `Parser`, `Formatter`, `Builder` | A stateless or short-lived transformation. |

This table is not an exhaustive suffix allowlist.
Names required by an external API and frontend-local native roles such as `Controller` still face the survival test, but this RFC does not add a suffix lint or demand renames for names that already describe a real boundary.

`Coordinator`, `Lifecycle`, `Dependencies`, `Context`, `Surface`, `Policy`, `Resolver`, and `Recommender` are not created by default and require an explicit justification.

- `Policy` requires an injectable strategy with multiple implementations. `TextOrderingPolicy` and `CompletionAliasPolicy` qualify; `ListActionPolicy` and `TrackColumnLayoutPolicy` are closed pure rules and join `ListActions` and `TrackColumnLayouts`.
- `Coordinator` requires coordinating two independent authorities. Grouping members is not coordination.
- `Dependencies` is permitted for an exact argument object consumed once by a constructor or factory. It may not be stored, queried, or re-exported as an ambient collaborator catalog.
- `Store` requires I/O. In-memory state with a signal is state or a model.
- `Context` is permitted only at a composition or build root and is field-count bounded.
- `Surface` denotes a real UI surface. `PlaybackCommandSurface` is not one and becomes `PlaybackActions`.
- `Lifecycle` is not a standalone class; a lifetime belongs to the thing whose lifetime it is.

The full renaming table is carried in the local execution plan rather than here, because a rename list is not a proposal-level fact.
Renames land with the consolidation that touches the file, never as a standalone rename sweep: a pure-rename change produces a large diff, zero structural gain, and a second diff later when the module actually merges.

### Localization

The three-layer text stack becomes one inventory, one catalog, and feature-local formatting.

1. Introduce `app/include/ao/i18n/MessageInventory.def` as the canonical two-field inventory, replacing 881 hand-written message enumerators and their 930-line hand-written key table; `Count` remains the enum terminator derived after the included entries.

   ```cpp
   AO_I18N_MESSAGE(TrackPresentationLibraryDescription,
                    "track_presentation_description_library")
   ```

   `MessageCatalog.h` includes it with a macro that emits the `MessageId` enumerators; private `MessageIds.h` includes it with a macro that emits `MessageDefinition` entries.
   Both fields remain explicit because the identifier is not derivable from the key: a snake-to-Pascal transform would produce `TrackPresentationDescriptionLibrary`, not the stable `TrackPresentationLibraryDescription` identifier.

   **Rule.** This change introduces no message-inventory generator or executable.
   `CatalogCompiler.cpp` already consumes `MessageIds.h`; making it generate its own input creates a bootstrap cycle and adds a tool concept to remove a data duplication.
   The `.def` source is ordinary compiler input, so a clean build has no generated-header ordering edge.
   `MessageIds.h` asserts that definition count equals `MessageId::Count` and that enum indices match inventory order. The catalog compiler sorts the inventory once, diagnoses duplicate keys, and then performs its exact-set comparison, so duplicates and any missing or extra ICU root resource fail the build without imposing an 881² constant-expression check on every consuming translation unit.
   Migrating the existing 881 pairs into the inventory is a one-time mechanical extraction, not a renaming.

   **Rule.** The inventory and its tooling visibility land atomically.
   `.def` becomes a governed C++ include-fragment suffix: `AssertNoForbiddenIncludes.cmake` scans it, changed-file discovery and `clang-format` select it, the name and architecture audits see it, and `clang-tidy` resolves it through a consuming translation unit as it does a header rather than trying to compile it alone.
   `script/ao/core/gitfiles.py` therefore separates formattable/include-fragment suffixes from standalone translation-unit suffixes instead of appending `.def` blindly to the current shared tuple.
   Phase 1b includes source-discovery tests for a changed `.def` file; a canonical source invisible to the normal hygiene path is not complete.
2. Delete both `GtkTextId` and `TuiTextId` with their parallel mapping arrays; frontend call sites use canonical ids directly.

   This deliberately removes compile-time totality for the two redundant frontend subsets. It is not replaced by a test-maintained prefix inventory: `gtk_*` and `tui_*` both contain argument-bearing messages that must use feature formatters. `gtkText` is only an owning GTK adapter over `requiredText`; `tuiChromeText` additionally supplies the fixed key arguments for TUI hints, help, and footers. Both otherwise retain the required-message fail-closed behavior, while feature-specific parameterized messages use named formatters.
3. Delete the `PresentationTextCatalog` class and publish two minimal helpers with fail-closed semantics.

```cpp
std::string_view requiredText(i18n::MessageCatalog const& catalog, i18n::MessageId id);
std::string requiredFormat(i18n::MessageCatalog const& catalog,
                           i18n::MessageId id,
                           std::span<i18n::MessageArgument const> arguments);
```

4. Keep semantic mapping as feature-local functions such as `formatLibraryScanMessage`, `formatListMembershipEditNotification`, `trackFieldLabel`, and `audioBackendPresentation`, without repackaging them as five `*TextCatalog` classes.

Until Phase 3 distributes those declarations into feature capsules, the neutral `PresentationText.h` umbrella may collect them; the deleted class name does not survive as a header name.

`WinUiResourceProjection` stays as a platform adapter.
Its positional-argument metadata and two property-qualified `x:Uid` aliases translate canonical catalog entries into native `.resw` constraints; they do not introduce a third semantic message-id enum.
English-only WinUI native diagnostics likewise stay outside the shared presentation catalog unless they become user-facing cross-frontend messages.

`ao_app_i18n_ordering` and `ao_app_i18n_completion` are both interactive ICU leaves separated by responsibility naming rather than by dependency direction, but merging them into one `ao_app_locale` is deferred until an AppKit frontend exists and its link subset is known.
One fewer archive is worth less than the CLI link-closure boundary enforced by `ao_cli_localization_boundary_check`, which stays either way, and a merge decided against two frontends may be wrong for three.

This work is sequenced early rather than late.
It is mechanical, changes no lifetime, changes no behavior, and `PresentationTextCatalog` is the first field of `GtkUiDependencies`, so removing it directly reduces the surface of the dependency-bag work that follows.
The [localization specification](../spec/presentation/localization.md) and the [text catalog reference](../reference/presentation/text-catalog.md) remain the authorities and gain the canonical inventory contract.

### Build and guardrail governance

Twenty-two boundary targets, seventeen generated regex files, and twenty-three recursive scanner invocations converge on one audit driven by a declarative rule table.

```cmake
aobus_attach_architecture_audit(
  TARGET ao_app_runtime
  PROFILE runtime)
```

One traversal reports every violation instead of failing on the first scanner.
The nine configure-time `message(FATAL_ERROR)` regex self-tests move to ordinary script unit tests and leave the configure path.

Enforcement uses the strongest mechanism available, in this order.

```text
private target/include visibility
  > private source/header placement
  > type capability
  > link-closure check
  > source regex scan
```

The honest limit of that ordering is that most current rules cannot be promoted.
While `ao_app_runtime` is one library, private target visibility cannot express "linux-gtk may not call `.writer()`", because GTK and the runtime's own implementation link the same archive, and this proposal explicitly refuses to split more static libraries to create the visibility.
So most boundary regexes stay and become regression insurance behind a stronger primary mechanism, rather than being replaced by the audit.

Exactly one rule is proposed for promotion: the live-runtime write-transaction owner, where a private header plus access structure can carry the boundary and the regex demotes to insurance.

**Rule.** The `LibraryWriter` clause is **retargeted, never deleted**.
An earlier draft of this analysis proposed removing it once the UIModel `ListAuthoring` module existed, on the reasoning that the rule would have nothing left to forbid.
That is wrong.
`ListAuthoring` removes the six forwarding methods without inventing an editor object, but it does not make a direct frontend call to the command surface impossible, and this proposal has already conceded that target visibility cannot express the boundary while `ao_app_runtime` is one archive.
Deleting the clause would trade six methods for the loss of all regression protection on the rule they were a symptom of.

`app/CMakeLists.txt:525` is therefore updated in the same change: `LibraryWriter` and `.writer(` become `LibraryCommands` and `.commands(`.
The rule count does not fall.
What falls is the number of production interfaces shaped by it, which is the actual cost this proposal is removing.
No aggregate audit framework solves that problem, and the audit consolidation must not be presented as if it does.

`app/CMakeLists.txt` splits by concern into `app/i18n/`, `app/runtime/`, `app/uimodel/`, `app/desktop/`, and `app/cmake/ArchitectureAudit.cmake`, while the coarse target set shrinks toward `ao_app_runtime`, `ao_app_uimodel`, `ao_app_locale`, and `ao_desktop_launch`.
`ao_app_desktop` contains only launch and switch helpers today and is renamed accordingly.
Source-file organization does not justify additional static libraries.

The [UIModel organization guide](../development/uimodel-organization.md) relaxes to top-level feature ownership: a cohesive module header may carry several related values and functions, implementation helpers need not be public, and tests are organized by behavior rather than mirrored one file per class.

### Phasing

Phase 0 establishes the baseline.

**Rule.** Header count is a secondary signal, not a success metric.
It is trivially satisfied by concatenating files, which can make the codebase worse: merging four small desktop headers would give `LibrarySwitch.h`'s 20 consumers the union of what `LibraryPath.h`'s 4 consumers see.
The primary metrics are the ones that cannot be gamed that way.

| Tier | Metric |
|---|---|
| Primary | Public declaration count (classes/structs, enums, aliases, free functions), not file count |
| Primary | Dependency edges between public concepts |
| Primary | Construction hops from a frontend component's `#include` to a usable instance |
| Primary | Transitive include weight and rebuild fan-out of `ao_app_uimodel` and `ao_app_runtime` |
| Secondary | Public header counts per area |
| Secondary | Role-suffix distribution and custom guardrail target count |
| Secondary | Field counts of every `*Context` and `*Dependencies` aggregate; count of types with default construction plus bind/unbind |
| Secondary | Clean and no-op build time |

Phase 0 makes the primary metrics reproducible from one configured debug build.
It extends the existing dependency-report path as `./ao deps report --concepts` rather than adding another top-level portal command, emits `concept-report.json` into the selected build tree, and reruns the same report after every phase.
The contributor procedure is owned by the [concept metrics guide](../development/concept-metrics.md).

| Primary metric | Measurement protocol |
|---|---|
| Public declarations | Walk the Clang AST for the self-contained headers under `include/ao` and `app/include/ao`, deduplicate canonical declarations by compiler identity, and count non-implicit public class/struct, enum, alias, free-function, and explicit public-member declarations. Each callable overload is one declaration whether it is a method or a free function, so moving behavior out of a wrapper class cannot regress the metric merely by changing ownership spelling. Core and application totals remain separate. The report records exclusions and overload treatment; a measurement correction increments the schema and rebaselines every compared phase rather than silently changing the denominator. |
| Dependency edges between public concepts | From the same AST, record one directed edge when a public member callable belongs to its owning public type, or when a public base, field, alias, parameter, or return type names another public declaration; deduplicate identical source-declaration/target pairs. A callable source retains its overload signature, matching the declaration denominator. The member-owner edge makes a method and an equivalent free function taking the owner symmetric. Header includes are reported separately and never substituted for this metric. |
| Construction hops | For every touched frontend leaf, record the before and after chain from its registration or composition-root entry through each aggregate lookup, factory, constructor, and required one-shot bind until the instance is usable. Each crossed API boundary is one hop; the per-change review names the chain rather than reporting only an aggregate. |
| Transitive include weight and rebuild fan-out | Use compiler dependency data to report distinct project headers and their total bytes reachable from each application public header, and use the Ninja dependency database to count translation units invalidated by each header. Retain per-header rows and report median and 95th percentile after building the same complete target set under the same preset in both revisions. When a phase changes the public-header set, compare percentiles over the shared-header cohort and list additions and deletions separately, so removing a low-fan-out header cannot move the denominator and masquerade as a regression. |

The Phase 0 report, its scope manifest, and its own fixture tests land before structural work.
Regex line counts are permitted only for the secondary inventory, never as a substitute for these four measurements.

A change that lowers a secondary metric while raising a primary one is rejected.

Phase 1 is restricted to changes with no lifetime effect, no behavior effect, and independent rollback.
Every session collapse moves to Phase 2, so that one lifetime change lands once instead of as two halves in two phases.

In the phase table, `Rule` is mandatory and requires an RFC amendment to change, `Design` is the recommended shape, and `Candidate` remains independently evidence-gated and may be dropped.

| Phase | Content | Tier | Precondition |
|---|---|---|---|
| 0 | Extend the existing dependency report with the AST, construction-chain, include-weight, and fan-out baseline above; freeze its scope and fixtures | Rule | None |
| 1a | Delete test-only `StrongTypeStream.h` with its structural test and the zero-reader `consecutiveFailureCount()` accessor; merge `AppPrefsState.h` and `AppStateStore.h` into `AppState`; move `persistentState` into the component descriptor and delete `StatefulLayoutComponentType` | Design | Phase 0 |
| 1a' | Merge the four desktop launch headers into `LibraryLaunch` | Candidate, gated on measured include fan-out for `LibrarySwitch.h`'s 20 consumers | Phase 0 |
| 1b | Introduce public `MessageInventory.def`, add governed include-fragment discovery for `.def`, derive `MessageId` and its key table from it, delete `GtkTextId` and `TuiTextId`, and delete the `PresentationTextCatalog` class in favor of `requiredText`/`requiredFormat` plus feature formatters | Design | Phase 0 |
| 1c | Adjudicated no-lifetime candidates from the repo-wide survey: constructor-close pure initial projection, remove remaining test-only production API, privatize test-only runtime counters and single-consumer utilities, inline duplicate audio bootstrap, absorb single-owner pure rules, and consolidate registration-only headers without merging implementation units | Candidate per item; transitive includes and behavior coverage gate each change | Phase 0 |
| 2 | Constructor-bind `ResourceByteLoader`, WinUI `TrackListController`, and `SmtcBridge`; delete `GtkUiDependencies` and move to registration-time capture; collapse the WinUI and GTK owner layers with their retirement protocols intact | Design | Phases 1a and 1b |
| 3 | UIModel feature capsules, including `ListPresentations` absorbing its lifecycle and recommender, and the new `ListAuthoring` functions that let `rt::Library`'s forwarding methods be deleted and the frontend guardrail clause retargeted in one change | Design | Phase 2 |
| 4 | Layout and shell: schema table, action and component schema merge, `LayoutSession`, registration-time capture in the build path | Design | Phase 2 |
| 5 | Runtime composition: `AppRuntime` owns `CoreRuntime` and forwards the audited application face, GTK preview construction moves behind `TrackSourceCache` and `ViewService`, virtual shutdown and protected initialization are removed, and resource loading moves out of `LibraryJobs` | Design | Phase 3 |
| 6 | One architecture audit, `app/CMakeLists.txt` split, target consolidation, naming and organization document reductions, and finally any architecture-document merges that lose their independent graph | Design | Phases 3-5 |

`TrackSourceLease` has been removed from Phase 1a; it is kept, per the survival-test discussion above.

Phases 1a' and 1c may proceed independently after Phase 0, but neither gates Phase 2.
Dropping an evidence-gated candidate therefore cannot leave a mandatory phase with an unsatisfied precondition.

Phase 1a is the cheap proof of the method rather than only a file shuffle: it removes one public header justified solely by its own test and one wholly unread accessor before larger feature consolidation begins.
If the Phase 0 report does not show the expected declaration and dependency-edge decrease without regressing construction hops or include metrics, later phases stop and the measurement or premise is corrected first.

Phase 1b is separated from 1a because it unlocks every later text change and carries a different review shape: it is canonical-inventory and mechanical-call-site review, not lifetime review.

Phases 3 and 4 are independent of each other once Phase 2 lands and should run concurrently.
Serializing them would delay the highest-value change behind the largest test-churn change: `test/unit/uimodel` holds 83 files and layout-related tests hold 58.

Documentation merges are deliberately last.
Whether [audio quality](../architecture/audio-quality.md) folds back into playback, whether [resource delivery](../architecture/resource-delivery.md) splits across library, playback, and presentation, whether [application shell](../architecture/application-shell.md) merges into presentation, and whether [interactive session lifecycle](../architecture/interactive-session-lifecycle.md) merges into system and persistence are all decided by the focused-architecture threshold in the [documentation system](../README.md#architecture-portfolio), applied to the code as it exists after Phase 5.

## Alternatives

**Do nothing.**
The layer map is correct and nothing is broken.
Rejected because the enforcement machinery has begun shaping production interfaces: `rt::Library` carries six methods whose only stated justification is a CMake regex, and an 881-entry enum inventory must be kept in sync with a separate 930-line key table by hand.

**One large refactoring change.**
Rejected because the highest-value area, layout and shell, is also the highest-risk and depends on the frontend dependency bags disappearing first.
A single change cannot be reviewed, and a partial revert would leave two half-vocabularies.

**Keep the roles and only shrink the naming document.**
Rejected because the duplication is in the code, not only in the vocabulary.
Shortening the naming document without deleting `ListPresentationPreferenceLifecycle` leaves the same concept count with less guidance.

**Replace the dependency bags with a service locator or DI container.**
Rejected explicitly.
A locator keeps every leaf's dependency set implicit and unverifiable at compile time, which is the property that makes `GtkUiDependencies` hard to reason about.
Registration-time capture makes each factory's dependency set exact and checked by the compiler.

**Extract a general framework for the recurring lane, generation, and session shapes.**
Rejected.
The mutation lane already refused a generic actor framework on the same grounds, and that judgment generalizes: a wrong shared lifetime is more expensive than a repeated one.

## Compatibility and migration

No user-visible behavior changes.
No persisted format changes: the [layout state](../reference/shell/layout-state.md), [presentation persisted state](../reference/presentation/persisted-state.md), [application config](../reference/persistence/application-config.md), [workspace session state](../reference/workspace/session-state.md), and [playback session state](../reference/playback/session-state.md) surfaces are unaffected.
The [CLI command surface](../reference/cli/command.md) and [TUI command surface](../reference/tui/command.md) are unaffected.

Internal source compatibility is deliberately not preserved; the project carries no migration constraint on internal interfaces.

Five ordering constraints are hard.

1. `rt::Library`'s list-mutation forwarders may only be deleted in the same change that introduces the UIModel `ListAuthoring` functions and retargets `AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX` from `LibraryWriter`/`.writer(` to `LibraryCommands`/`.commands(`. Any other order either breaks the GTK build or drops the boundary.
2. The layout build path may only lose its dependency aggregate after the aggregate itself is deleted in Phase 2.
3. The canonical `MessageId` inventory and `.def` source-discovery support must land before or with the `PresentationTextCatalog` deletion so that no intermediate state has two frontend id spaces and no catalog wrapper, and no canonical inventory is invisible to guardrails or changed-file hygiene.
4. The complete `AppRuntime` forwarding face must land in the same change that removes the inheritance: both `library()` overloads, `async()`, `sources()`, `notifications()`, `completion()`, `textOrderingPolicy()`, and `musicRoot()`. The same change must remove the two GTK `musicLibrary()` calls by moving preview evaluation and projection construction behind their existing runtime owners. It must also migrate the broader test-fixture surface: path assertions use `musicRoot()`, storage checks use an explicit `CoreRuntime`/`MusicLibrary` fixture or the existing runtime owner, and projection tests enter through `TrackSourceCache` and `ViewService` or a runtime-private fixture. Tests must not preserve `AppRuntime` as a `MusicLibrary` entry through a test-only forwarder or a `core()` escape hatch. `musicLibrary()` and `databasePath()` are not forwarded; without all sides of the change, Phase 5 either becomes an accidental whole-application rewrite or preserves the raw-storage leak it exists to close.
5. GTK layout generation replacement must preserve the single parent of `TrackPageHost::stack()`. `LayoutHost::prepare()` builds the successor before `commit()` destroys the predecessor, so detaching only in the semantic component destructors is too late. Phase 4 must make the handoff explicit and preserve prepare-failure rollback; a failed successor build must leave the active generation parented and usable.

Platform coverage: the WinUI session collapse and the `TrackListController` and `SmtcBridge` constructor binding require native Windows validation per the [Windows development guide](../development/windows.md).
That validation must confirm that the shell-persistent WinUI controls still rebind across layout rebuilds, because this proposal removes constructor-once binding for one class of type and deliberately preserves it for another.
It must also confirm that retiring SMTC still disables native controls, revokes the button event, cancels artwork work, and runs before the shared resource loader is released.
The GTK session and layout work requires the lifetime review in the [GTK lifetime guide](../development/gtk-lifetime.md), because deleting bind protocols changes when subscriptions are established relative to widget construction.
Its layout replacement coverage must switch between layouts containing `track.table` and `workspace.withDetailPane`, and must also force a failed prepared build after the shared stack handoff point, proving both single-parent attachment and rollback.

## Validation

Each phase runs the full gate defined by the [testing policy](../development/test.md) and the [validation and review guide](../development/test/validation-and-review.md): one complete `./ao check`, then `./ao hygiene`.
Changes touching subscription teardown or generation gates additionally run `./ao test --concurrency` and the sanitizer suites named in the [concurrency and sanitizer guide](../development/test/concurrency-and-sanitizer.md).

Behavior coverage is preserved rather than transferred.
When a class disappears, its behavior tests move to the surviving owner's test file; only tests that exist solely to mirror a deleted class structure are deleted.
Per the [layer selection guide](../development/test/layer-selection.md), consolidation must not push a UIModel behavior test into a frontend suite.

Acceptance criteria for the proposal as a whole:

| Metric | Target |
|---|---|
| Public declarations in the application layer | About 25 percent fewer |
| Application public role nouns | About 30 percent fewer |
| Core and tool declaration count | No percentage target; only unused surface or roles that fail the survival test are removed |
| Construction hops from a component `#include` to a usable instance | Strictly lower for every touched component |
| Transitive include weight and rebuild fan-out for the shared-header cohort | No increase at the median or 95th percentile; added and removed headers are named separately |
| Application public headers (secondary) | Falls, but never at the cost of a primary metric |
| `Lifecycle` as a standalone type | Zero |
| `Coordinator` types that group members without coordinating two independent authorities | Zero |
| `*Dependencies` aggregates used as an ambient service locator | Zero |
| Stored or re-exported nullable collaborator aggregates | Zero; exact one-shot constructor/factory arguments, including explicitly optional nullable collaborators, are permitted when the bag itself is immediately consumed |
| Types with `bind`/`unbind` and no demonstrated replacement scenario | Zero |
| Duplicated frontend message-id spaces | Zero |
| Architecture source-scan targets | One aggregate audit plus a small number of platform-specific checks |
| Public wrapper over a single STL type | Must prove an additional invariant or be deleted |
| Production header whose only consumer is a test of that header's otherwise-unused API | Zero |
| Public test-observability counter API with no production reader | Zero; deterministic assertions survive behind source-private probes |
| Generic utility | At least three production consumers with identical semantics |

The `Coordinator` row is deliberately not "zero `Coordinator` types".
Eight remain in the tree, including `ThemeCoordinator`, `ImportExportCoordinator` (which implements `ImportExportActions`), `LibraryTransferCoordinator`, `ListAuthoringCoordinator`, and `TrackPropertiesCoordinator`, and some of those genuinely coordinate independent authorities.
A zero target would contradict this proposal's own rule that `Coordinator` is legitimate in that case, and would license exactly the pure-rename changesets the proposal forbids.
What must reach zero is the pattern, not the suffix.

These are directional targets for review, not a new lint.
The operative review questions are three: what does it own, what does it guarantee beyond the types it holds, and which correctness contract is lost if it is deleted.
A public role for which none of those questions has an answer is absorbed or deleted; a negative answer to one question is not a score or an automatic verdict.

## Open questions

None.
The proposal-level choices are closed; implementation alternatives explicitly permitted by the design tiers are adjudicated within their phases and do not reopen acceptance.

## Promotion plan

As implementation lands, this RFC updates or creates the following authorities and is then deleted.

| Document | Change |
|---|---|
| [System architecture](../architecture/system-overview.md) | Replace the `AppRuntime : CoreRuntime` relationship with composition; restate the composition-root collaborator rule. |
| [Application shell architecture](../architecture/application-shell.md) | Replace the layout state stack and the frontend dependency-aggregate model with `LayoutSession` and registration-time capture. |
| [Interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) | Reduce the number of owners expressing one session; restate the terminal retirement protocol against the surviving owner without weakening it. |
| [Library architecture](../architecture/library.md) | Rename the four Library roles, remove the frontend forwarding seam, and restate the frontend write-authority boundary against `LibraryCommands`. |
| [Presentation architecture](../architecture/presentation.md) | Restate UIModel granularity as feature capsules and remove the catalog wrapper layer. |
| [Architecture landscape](../architecture/README.md) | Update relationship and coverage rows for every architecture changed above. |
| [Layout lifecycle specification](../spec/shell/layout-lifecycle.md) | Restate generation and component-state ownership against `LayoutSession`. |
| [Localization specification](../spec/presentation/localization.md) | Restate the fail-closed lookup contract as `requiredText` and `requiredFormat`. |
| [Component vocabulary reference](../reference/shell/component-vocabulary.md) | Become generated output of the shared schema table; add `persistentState`. |
| [Text catalog reference](../reference/presentation/text-catalog.md) | Declare `ao/i18n/MessageInventory.def` canonical and remove the frontend id spaces. |
| [Naming convention](../development/naming-convention.md) | Reduce from a suffix encyclopedia to principles, a decision tree, and the reduced role table. |
| [UIModel organization guide](../development/uimodel-organization.md) | Relax file-for-file organization to top-level feature ownership. |
| [Application layer review guide](../development/application-layer-review.md) | Adopt the three review questions only. The five-question form and any scoring table stay out of the guide and are deleted with this RFC. |
| Decision record | Record the composition-over-inheritance runtime choice and the rejection of a service locator, if that rationale remains useful after implementation. |
