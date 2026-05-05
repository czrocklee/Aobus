# Aobus Shared App Runtime Architecture

> **Audience:** This document is for contributors designing application architecture, frontend shells, and future customization surfaces.
>
> **Purpose:** Define the target top-level runtime architecture that allows Aobus to support multiple native UI shells, repeated built-in widgets, and controlled UI customization without coupling application logic to a specific UI toolkit.
>
> **Out of scope:** This document intentionally does **not** discuss migration strategy, rollout sequencing, backward compatibility, or implementation steps.

---

## 1. Motivation

Aobus currently has a Linux GTK shell, but the product direction requires a more general foundation.

The architecture must support all of the following at the same time:

- a GTK shell today and a WinUI3 native shell later
- multiple renderers of the same capability, such as two playback bars bound to the same playback session
- UI composition that is more flexible than a single hard-coded main window layout
- strong product quality control, with curated built-in capabilities rather than arbitrary end-user scripting
- continued use of strong C++ types as the primary design language

The current architecture already contains solid lower-level building blocks:

- audio and backend coordination are already mostly toolkit-neutral
- cross-thread delivery already goes through a UI-neutral dispatcher interface
- library list membership already uses efficient observer-based propagation

The main limitation is at the application orchestration layer. Today, too much behavior is wired together through shell-specific coordinators and central window callbacks. That works for one shell with one canonical layout, but it scales poorly when the product needs:

- multiple frontend implementations
- multiple instances of the same widget capability
- view-local state that is independent across several simultaneous views
- a stable contract for future curated extensibility

The design goal is therefore not to introduce a single magic bus that replaces all existing patterns. The goal is to introduce a **shared application runtime** that becomes the single UI-neutral control surface for shells and built-in components.

---

## 2. Design Goals

The shared runtime must satisfy these goals.

### 2.1 UI-library neutrality

The application core must not depend on GTK, WinUI3, GObject, C++/WinRT, or any other UI framework type.

### 2.2 Strong C++ contracts

Cross-component communication must use explicit C++ message and state types, not string topics, reflection-heavy payload bags, or dynamic script objects.

### 2.3 Repeated built-in widgets

Any built-in UI capability should be renderable more than once. Two playback bars must be a normal supported case, not a special-case hack.

### 2.4 Scoped state

The architecture must distinguish between session-global state and view-local state. A playback state is global to the session. A track list filter and selection are local to a view instance.

### 2.5 Controlled customization

Customization should compose curated built-in capabilities. The default product model is not to let end users write arbitrary imperative code against unstable internals.

### 2.6 Clear ownership

The runtime must own application state and domain coordination. Shells must own widget instances, visual presentation, toolkit gestures, and layout.

### 2.7 Respect existing threading constraints

The design must remain compatible with the core threading rule documented in [THREADING.md](THREADING.md): the audio I/O path must never be burdened with broad UI fan-out or toolkit coupling.

---

## 3. Non-goals

This architecture explicitly does not aim to do the following:

- define a migration path from the current GTK coordinators
- preserve current shell assumptions for compatibility reasons
- expose a user-facing scripting language
- turn every collection update into a generic global bus event
- replace efficient domain-specific observer mechanisms where they are already the right fit
- make the audio realtime path publish directly to arbitrary UI subscribers

---

## 4. Architectural Overview

At the top level, Aobus should be split into a shared runtime and one or more frontend shells.

```text
╭──────────────────────────── Frontend Shell ────────────────────────────╮
│ GTK Shell / WinUI3 Shell / future native shell                         │
│                                                                        │
│ Built-in widgets                                                       │
│ - PlaybackBar                                                          │
│ - TrackListView                                                        │
│ - Inspector                                                            │
│ - StatusArea                                                           │
│ - Navigation components                                                │
│                                                                        │
│ Responsibilities                                                       │
│ - layout and widget lifetime                                           │
│ - toolkit gestures and visuals                                         │
│ - binding widgets to runtime stores and projections                    │
│ - translating user interaction into runtime commands                   │
╰───────────────────────────────┬────────────────────────────────────────╯
                                │
                                ▼
╭────────────────────────── Shared App Runtime ──────────────────────────╮
│ AppSession                                                             │
│ - CommandBus                                                           │
│ - EventBus                                                             │
│ - Stores                                                               │
│ - Projections                                                          │
│ - Services                                                             │
│                                                                        │
│ Responsibilities                                                       │
│ - application state ownership                                          │
│ - command handling                                                     │
│ - event publication                                                    │
│ - view registration and scoped state                                   │
│ - playback orchestration                                               │
│ - library mutation coordination                                        │
╰───────────────────────────────┬────────────────────────────────────────╯
                                │
                                ▼
╭──────────────────────────── Domain Layer ──────────────────────────────╮
│ Player / Engine / backend providers / MusicLibrary / SmartListEngine  │
╰────────────────────────────────────────────────────────────────────────╯
```

This split makes the runtime the stable semantic center of the app, while each shell becomes a rendering and input adapter.

---

## 5. Core Runtime Model

The shared runtime is centered on a session-scoped object.

### 5.1 `AppSession`

`AppSession` is the root of one active application session.

Conceptually, one `AppSession` owns:

- the active library context
- playback orchestration for that library
- global application stores
- registered view instances and their local state
- command and event routing
- domain-facing services

`AppSession` is deliberately **not** a process-global singleton.

This is important for three reasons:

- it keeps tests isolated
- it keeps future multi-window or multi-session possibilities open
- it avoids hiding ownership behind global mutable state

An `AppSession` represents one coherent runtime world. Shell windows, panes, or surfaces bind to that session rather than owning the business logic themselves.

### 5.2 Conceptual shape

The following sketch is illustrative, not normative.

```cpp
namespace ao::app
{
  using ViewId = ao::StrongId<struct ViewTag, std::uint64_t>;

  class AppSession final
  {
  public:
    IControlExecutor& executor();
    CommandBus& commands();
    EventBus& events();

    IReadOnlyStore<PlaybackState>& playback();
    IReadOnlyStore<FocusState>& focus();
    IReadOnlyStore<NotificationFeedState>& notifications();

    ViewRegistry& views();
    LibraryQueryService& queries();
    NotificationService& notificationService();
  };
}
```

The exact class names may evolve, but the architectural roles should remain stable.

---

## 6. Communication Model

The runtime uses four different communication shapes, each with a specific role.

### 6.1 Commands

Commands represent **intent**.

Examples:

- `PlayTrack`
- `PlaySelectionInView`
- `PausePlayback`
- `SeekPlayback`
- `SetOutputDevice`
- `UpdateTrackMetadata`
- `EditTrackTags`
- `OpenListInView`
- `CreateView`
- `DestroyView`

Rules:

- a command has one authoritative handler inside the runtime
- commands enter the runtime from shells, built-in components, or curated internal extensions
- commands are typed C++ values
- commands express semantics, not widget gestures

Rationale:

- intent needs a clear owner
- multiple handlers for one command create ambiguity and race-prone behavior
- shell input should be translated into application-level meaning before it enters the runtime

### 6.2 Events

Events represent **facts that already happened**.

Examples:

- `PlaybackStarted`
- `PlaybackStopped`
- `NowPlayingChanged`
- `PlaybackOutputsChanged`
- `TracksMutated`
- `LibraryImported`
- `NotificationPosted`
- `RevealTrackRequested`

Rules:

- any number of subscribers may observe an event
- events are descriptive, not imperative
- publishing an event must not require knowledge of who listens to it

Rationale:

- several independent components may need the same fact
- events are good for fan-out, logging, shell reactions, and diagnostics
- facts should remain decoupled from a particular frontend layout

### 6.3 Stores

Stores represent **current state**.

Examples:

- `PlaybackStore`
- `OutputStore`
- `FocusStore`
- `NotificationStore`
- per-view `ViewStateStore`

Rules:

- a store always exposes the current snapshot
- subscribers must be able to join late and still obtain the current state
- stores are owned and updated by runtime services, not by arbitrary widgets

Rationale:

- repeated widgets need immediate current state, not just event history
- two playback bars are naturally modeled as two renderers bound to the same playback store
- state snapshots make shells simpler and more deterministic

### 6.4 Projections

Projections represent **structured read models** for larger or more dynamic datasets.

Examples:

- track list membership for a view
- list tree hierarchy
- track details for an inspector
- filtered or grouped result sets

Rules:

- projections are UI-neutral
- projections may expose delta-based subscription rather than full-state replacement
- projections remain typed and domain-aware

Rationale:

- large collections are a poor fit for a naive global event stream
- the library already uses efficient list observers for membership changes
- projections preserve performance and structure where a generic bus would be too blunt

The bus is the foundation for orchestration. It is **not** the only abstraction in the runtime.

### 6.5 Runtime execution and delivery semantics

The runtime is intentionally designed around a **serialized control context**.

Conceptually:

- each `AppSession` owns one control-plane execution context
- in native shells, that control context will usually be the same thread targeted by the existing [`IMainThreadDispatcher`](../../include/ao/utility/IMainThreadDispatcher.h)
- commands are handled on that control context
- stores are mutated only on that control context
- store subscribers and event subscribers are invoked on that control context

This means stores are not intended to be modeled as free-threaded, many-writer data structures. Their concurrency rule is simpler and stricter: they are **control-context affine**.

Design reason:

- a single serialized control context keeps state ownership clear
- it avoids hidden races between UI state, domain reactions, and shell rendering
- it remains compatible with the threading constraints in [THREADING.md](THREADING.md)

Cross-thread producers such as:

- audio callbacks
- backend monitor threads
- import or export workers
- metadata workers

must never mutate runtime stores directly. They must first marshal a state transition or callback onto the `AppSession` control context through the dispatcher bridge.

This rule is especially important for the audio path:

- the audio I/O thread must not fan out directly to arbitrary UI or runtime subscribers
- the audio I/O thread may only emit coarse state transitions into the dispatcher bridge
- runtime store updates happen later on the control context

Unless a specific store explicitly documents otherwise, direct subscription callbacks are guaranteed only on the runtime control context. If an implementation later offers cross-thread snapshot access, that should be treated as a separate copy-based convenience API rather than the default semantic contract.

### 6.6 Command completion and acknowledgement

Commands are **not** implicitly fire-and-forget.

At the architectural level, every command has a completion contract even if many callers choose to ignore it.

Conceptually:

- a command declares a typed reply payload, which may be empty
- command completion resolves as `Result<Reply>`
- completion is delivered on the runtime control context
- success is explicit
- failure is explicit
- failure is never inferred from the absence of a success event

Commands dispatched from the control context are **synchronous**: the handler executes and `Result<Reply>` is available before `execute()` returns. Cross-thread callers marshal onto the control context through `IControlExecutor::dispatch` before calling `execute()`.

Design reason:

- a single serialized control context makes synchronous execution the natural default
- user-initiated edits (metadata, tags, pause, seek) need direct acknowledgement without callback overhead
- optimistic UI updates require an explicit rollback path: the caller inspects `Result` and reverts immediately

#### 6.6.1 Long-running and asynchronous work

Commands whose handler performs long-running or out-of-process work must **not** block the control context. The pattern is:

1. The command handler acknowledges receipt synchronously — `execute()` returns immediately with a receipt reply (e.g. `acknowledged = true`).
2. Real work is dispatched to a worker thread or external process.
3. Progress and completion are reported through **events** on the control context.
4. Shells observe results via event subscriptions.

Example:

- `ImportFiles` returns `ImportFilesReply{acknowledged}` synchronously
- the import worker runs on a background thread
- the worker dispatches `ImportProgressUpdated` events for progress
- on completion, the worker dispatches `LibraryImportCompleted` with the final count

This yields a clear distinction across three completion shapes:

- **command completion** tells the initiator whether the request was accepted
- **events** tell the rest of the runtime what facts became true afterward (including async completion)
- **stores** expose the new steady-state if the operation succeeded

---

## 7. Why Not One Global Bus

The runtime intentionally separates commands, events, stores, and projections instead of collapsing everything into one universal message system.

### 7.1 A single bus is too ambiguous

Intent, fact, and current state are different concepts. Merging them makes the API harder to reason about and easier to misuse.

### 7.2 Stores solve repeated-widget rendering cleanly

Two playback bars should not need to replay an event history to discover current playback state. They should bind to the same current state.

### 7.3 Large collections need richer structure

Track lists, smart lists, and grouped results need domain-aware projections and efficient deltas. A generic event bus is not the right abstraction for those shapes.

### 7.4 Strong typing matters

The runtime should not become a string-topic dispatch layer with loosely typed payloads. Aobus is a strong C++ application, and the architecture should take advantage of that.

---

## 8. Scope Model

Supporting flexible UI composition requires explicit state scopes.

### 8.1 Session-global scope

Session-global state is shared by all renderers within an `AppSession`.

Examples:

- current playback state
- output device inventory
- now-playing context
- library summary
- notifications

If two playback bars exist, they both bind to the same session-global playback state.

### 8.2 View-local scope

View-local state belongs to one specific logical view instance.

Examples:

- which list a track view shows
- that view's filter expression
- that view's grouping and sorting
- that view's current selection
- that view's scroll or presentation preferences

This scope exists so the app can support multiple track views at once without hidden interference.

### 8.3 Shell-local scope

Some state must remain outside the shared runtime because it is purely a shell concern.

Examples:

- popover anchor widgets
- splitter positions specific to one shell surface
- transient toolkit focus handles
- theme reload mechanics

This separation keeps the runtime clean and UI-neutral.

### 8.4 `ViewId`

Each logical view that owns independent local state must have a stable typed identity.

`ViewId` is the runtime key for:

- querying the view's local state
- targeting commands at a specific view
- connecting shell widgets to the correct projection

This replaces shell-specific assumptions such as “the currently visible page” with explicit, stable semantics.

### 8.5 Logical view lifecycle

`ViewId` identifies a **logical view**, not a specific toolkit widget instance.

The runtime therefore distinguishes between view state and widget state.

A logical view should move through explicit lifecycle states:

- **Attached** — a shell widget is currently bound to the view
- **Detached** — the logical view still exists, but no widget is currently attached
- **Destroyed** — the logical view is retired and its runtime-owned state may be released

Examples of a detached view include:

- a temporarily hidden tab
- a shell layout rebuild that tears down and recreates widgets
- view virtualization where a logical surface remains alive while its current native widget does not

This is the intended meaning of "outlive individual toolkit widgets when necessary".

Design reason:

- view-local state often needs to survive temporary shell teardown
- widget lifetime and logical view lifetime are not always the same thing
- preserving this distinction lets shells control whether closing a surface means hiding it or destroying it

The runtime should follow these rules:

- reopening a detached logical view reuses the same `ViewId`
- destroying a logical view retires that `ViewId`
- `ViewId`s are never silently recycled within the same session
- opening a genuinely new view creates a new `ViewId`, even if it points at the same list as another view

If a shell wants close-and-later-restore semantics after a view has been destroyed, that should be modeled as restoration from an explicit persisted view snapshot, not by silently resurrecting or recycling the old `ViewId`.

---

## 9. Runtime Services

Services are the only runtime components allowed to mutate authoritative state.

### 9.1 Playback service

The playback service owns application-level playback coordination.

Responsibilities:

- accept playback commands
- coordinate `Player` and playback sequence state
- publish playback-related events
- update the playback store
- maintain the relationship between now-playing track, source context, and output routing

Design reason:

- playback must not be controlled by whichever widget currently appears “primary”
- any number of transport widgets must be able to drive the same session
- the shell should not own playback truth

### 9.2 Library mutation service

The library mutation service owns all write-side mutation coordination.

Responsibilities:

- execute metadata and tag changes
- run import or mutation transactions
- invalidate relevant runtime projections and caches
- publish mutation events
- update affected stores or projections

Design reason:

- every mutation entry point must obey the same post-write rules
- inline editing, inspector editing, tag editing, and future curated extensions must not each invent their own cache invalidation path
- a single write pipeline improves consistency and testability

### 9.3 Library query service

The library query service owns read-side access to the library in UI-neutral terms.

Responsibilities:

- create projections for list views and detail views
- expose track details and list membership in toolkit-neutral shapes
- provide view-targeted read models

Design reason:

- shells need structured data, not direct ownership of domain internals
- read-side concerns should be centralized and reusable across GTK and WinUI3

### 9.4 View registry

The view registry owns logical view lifetime inside the runtime.

Responsibilities:

- allocate and retire `ViewId`s
- store view-local state
- associate a view with its current list target and projection
- enable components to target commands or queries to a specific view

Design reason:

- repeated list views are a first-class requirement
- logical view identity must outlive individual toolkit widgets when necessary

### 9.5 Notification service

The notification service owns user-visible app messages at the runtime level.

Responsibilities:

- publish informational, warning, and error notifications
- provide a store or feed that shells can render as status bars, banners, or toasts

Design reason:

- shells should choose how to render messages
- services should not hard-code a specific GTK-style status banner model
- notifications are user-facing presentation policy, not the sole transport for operational failure state

### 9.6 Fault and error reporting model

The runtime should treat errors and faults as a layered communication problem rather than forcing every failure through one mechanism.

There are four distinct needs:

- the initiating caller may need an immediate success or failure result
- the wider runtime may need to know that a subsystem changed state
- the current steady-state may need to represent a degraded or failed condition
- the shell may need a user-visible message to render

Those needs map to four mechanisms:

- **command completion** for initiator-local acknowledgement
- **events** for fault transitions or notable failures observed by multiple listeners
- **stores** for steady-state degraded conditions
- **notification service** for user-facing presentation

This means notification delivery is important, but it is not the authoritative source of truth for whether an operation failed.

Examples:

- a failed `UpdateTrackMetadata` command returns an explicit error completion to the initiating editor; the editor rolls back its optimistic UI state
- a lost output device updates the playback store to a degraded state, may publish a playback-fault transition event, and may also post a notification for the shell to render
- a library import failure may fail its command completion, publish an import-failed event for observers, and add a user-facing message through the notification service

Design reason:

- local rollback and global awareness are different concerns
- long-lived degraded state belongs in stores, not only in transient events
- shell presentation should remain decoupled from operational error transport

---

## 10. Store Model

Stores should prefer semantic state over presentation-ready text.

### 10.1 Semantic state first

The runtime should publish data such as:

- transport enum
- current track ID
- playback position and duration
- selected output device and profiles
- quality enum and diagnostics
- subsystem fault or degraded-state diagnostics where relevant
- focused view ID

The runtime should avoid hard-coding too much presentation text as the primary contract.

Design reason:

- GTK and WinUI3 may render the same semantics differently
- localization and shell-specific presentation remain easier when the runtime exports meaning rather than formatting
- semantic state is more stable than UI text fragments

### 10.2 Authoritative update path

Only runtime services update stores. Widgets and shells never directly mutate store state.

Design reason:

- state ownership must remain centralized
- arbitrary UI-side mutation produces inconsistent truth and race-prone interactions

### 10.3 Late subscription

Every store must support late subscribers without requiring event replay.

Design reason:

- a repeated widget can be added after playback is already active
- a freshly mounted shell component must still render the correct current state immediately

### 10.4 Thread-affinity rule for stores

Stores are mutated only on the `AppSession` control context described in Section 6.5.

Subscriber callbacks are delivered on that same control context.

Design reason:

- subscribers must observe state transitions in a deterministic order
- UI shells need a clear callback thread contract
- the runtime must avoid pushing multi-subscriber delivery work into audio or worker threads

In other words, the store contract is based on serialized ownership, not on broad shared-memory concurrency.

---

## 11. Projection Model

The runtime needs a richer abstraction than stores for track collections and large structured data.

### 11.1 Track list projections

A track list view should bind to a projection keyed by `ViewId`.

That projection provides the ordered TrackId sequence and structural metadata. Row presentation data (text, numeric properties) is not part of the projection — shell adapters read it directly from `TrackView` and resolve dictionary-backed text via `DictionaryStore::get()`.

### 11.2 Detail projections

An inspector or metadata panel should bind to a detail projection based on either:

- an explicit target view
- the current focused view
- a specific track selection context

### 11.3 Why projections remain separate

Design reason:

- projections preserve shape and intent for structured data
- projections can optimize for delta updates without forcing the entire app through one generic bus abstraction
- existing list observer concepts remain architecturally valuable even in the new runtime model

### 11.4 Projection-to-shell binding strategy for large libraries

For very large libraries, the runtime must avoid a design that copies an entire projection into a second generic UI-neutral row array and then copies it again into toolkit-native objects.

The intended strategy is:

- the runtime projection owns **ordering, membership, and identity**, not a fully materialized UI row object for every track
- large list projections are primarily sequences of stable IDs plus projection metadata such as grouping or section boundaries
- shells build a **thin native adapter** directly on top of that projection
- row snapshots are materialized only when a shell requests a specific visible index or small range

In practical terms, a projection for a large track list should primarily answer questions such as:

- how many items exist
- which `TrackId` is at index `N`
- which contiguous ranges were inserted, removed, or invalidated
- how to fetch the current display snapshot for a specific `TrackId` or visible range

It should not eagerly allocate one heavyweight presentation object per row for the entire library.

Design reason:

- the library and query layers already operate efficiently on `TrackId` plus zero-copy `TrackView` access
- the expensive part is usually row materialization and toolkit object creation, not membership identity itself
- repeating a full mirror in an intermediate runtime layer would multiply memory cost without adding semantic value

### 11.4A Sorted projections may eagerly materialize order metadata

For sorted or grouped views, the runtime should not pretend that everything can remain lazily unordered forever.

If a view requires a global ordering, the projection is allowed and expected to eagerly materialize a compact **order index** for the full projection membership.

That order index is conceptually different from a fully materialized UI row table.

It should contain only the data required to maintain ordering and grouping efficiently, such as:

- `TrackId`
- numeric sort fields such as year, disc number, track number, duration
- compact grouping keys
- dictionary-backed metadata identifiers or precomputed dictionary sort ranks
- normalized or compact sort keys for non-interned text fields such as title when needed

This distinction is critical:

- **eager order metadata for all items** is acceptable for a sorted projection
- **eager full UI row materialization for all items** is not the architectural default for large-library binding

Design reason:

- a globally sorted projection needs stable ordering data for its whole membership
- the current data model already stores many sort-relevant fields compactly in hot storage
- maintaining a compact order index is far cheaper than instantiating a million toolkit-facing row objects

In other words, the large-library target is not "fully lazy everything". It is "eager where ordering requires it, lazy where presentation does not".

### 11.5 Delta vocabulary must match native list models

To avoid expensive bridge translation, projection deltas should be expressed in a form close to what native list controls already want.

The preferred delta vocabulary is:

- `reset`
- `insert_range { start, count }`
- `remove_range { start, count }`
- `update_range { start, count }`
- optional future move or regroup operations if a shell can exploit them efficiently

The runtime should prefer contiguous range deltas over unordered per-item patch streams.

Design reason:

- GTK and WinUI-style list models are index-oriented
- bridging a generic unordered delta into native APIs usually forces extra diff work in the shell adapter
- range deltas minimize adapter-side translation and object churn

This also implies a practical rule:

- when the runtime can cheaply express a localized change as ranges, it should do so
- when a change is broad enough that a minimal diff is more expensive than the UI benefit, the runtime may intentionally emit `reset`

Examples of `reset`-worthy operations include:

- replacing the active filter with a very different query
- changing a grouping mode that reshapes the entire ordered result set
- switching a view to a different logical source list

Examples of range-friendly operations include:

- inserting imported tracks into the base ordered list
- removing a known set of tracks
- invalidating rows after metadata edits that do not reorder the projection

Design reason:

- million-item projections cannot afford maximal-diff computation on every broad reshape
- shells should optimize for the common localized case and accept `reset` for full-structure transitions

### 11.6 Row materialization should be lazy and bounded

Row presentation data should be materialized on demand from the underlying library data plane.

The intended pipeline is:

- projections hold ordered IDs and structural metadata
- when the shell needs to render a visible item, the adapter asks for a row snapshot for that ID or visible range
- the runtime or shell-facing adapter resolves the necessary hot and cold fields only for those requested rows
- resolved row snapshots are cached with a bounded policy rather than retained forever for the whole library

This design is grounded in the existing library model:

- LMDB-backed `TrackView` already provides zero-copy access to binary data
- the hot/cold split already separates frequently queried fields from colder payloads
- query compilation already tracks whether a smart-list expression needs hot data, cold data, or both

Those properties suggest that large projections should stay as close as possible to `TrackId` plus `TrackView` until a shell actually needs presentation data.

An eager shell-local full-row cache may still exist as an implementation detail for small or medium datasets, but it is not the architectural contract for large-library projection binding.

Design reason:

- it preserves the value of the current hot/cold data model
- it avoids building a million-row presentation cache eagerly
- it makes repeated shells or repeated widgets share the same domain result set without duplicating full row objects

### 11.6A String ownership and conversion strategy

The shared runtime must remain UI-neutral, which means it cannot adopt `Glib::ustring` or any other toolkit-native string wrapper as a core architectural type.

At the same time, the runtime should avoid needless string duplication for large projections.

The intended string strategy is therefore tiered.

#### Canonical runtime forms

Inside the shared runtime, text should prefer these forms:

- UTF-8 bytes in standard C++ string storage for non-interned fields
- interned identifiers such as `DictionaryId` for dictionary-backed metadata
- optional compact normalized or collation keys for sorting, distinct from display text

The runtime should explicitly separate:

- **display text**
- **sort key**
- **group key**

Those are related, but they are not the same artifact.

Design reason:

- sorting does not require toolkit-native strings
- dictionary-backed metadata should not be expanded into duplicated display strings for every row if an identifier will do
- normalized sort keys can be much more compact and more stable than repeated UI string wrappers

#### Dictionary-backed fields

Fields such as artist, album, genre, album artist, composer, and work already have a natural compact identity through dictionary storage.

For those fields, projections should prefer to retain:

- the dictionary-backed identifier for display lookup
- a precomputed lexical rank or collation key for ordering when needed

Shell adapters may then cache toolkit-native string objects such as `Glib::ustring` or `hstring` by dictionary identity.

Design reason:

- these values repeat heavily across the library
- repeated toolkit string conversion is wasteful when one adapter-local cache can serve many visible rows

#### Non-interned fields

Fields such as title are different because they are not naturally dictionary-backed and may be unique per track.

For those fields, the runtime may still need eager all-item sort metadata in a sorted projection, but that does not imply eager all-item toolkit string conversion.

Acceptable runtime shapes include:

- compact UTF-8 storage used directly as the canonical display text
- compact normalized sort keys stored separately from the display bytes
- immutable shared string handles if an implementation finds them beneficial

The shell adapter should convert those bytes into toolkit-native string wrappers only for visible rows or bounded caches.

Design reason:

- title sorting may require eager all-item ordering information
- title rendering does not require eager all-item native string allocation

#### Shell-native string caches remain shell-local

If GTK benefits from caching `Glib::ustring` and WinUI3 benefits from caching `hstring`, those caches should live in the shell adapter layer, not in the shared runtime contract.

This preserves two goals simultaneously:

- the runtime stays UI-neutral
- each shell can still optimize away repeated string conversion for its own toolkit

The current GTK approach of eagerly materializing `Glib::ustring` into row objects is a shell optimization for the current architecture, not the target shared-runtime contract.

### 11.7 Shell adapters should remain toolkit-specific

The shared runtime owns projection semantics. The final adapter that binds a projection to a native list control remains shell-specific.

Examples:

- a GTK shell may expose a custom `Gio::ListModel` backed directly by a projection
- a WinUI3 shell may expose a native items-source adapter backed by the same projection

What matters architecturally is that both adapters follow the same contract:

- query size from the projection
- translate range deltas directly into native list notifications
- fetch row snapshots lazily by index or visible range
- keep only a bounded cache of native row wrapper objects

This is preferable to forcing the shared runtime to invent a pseudo-toolkit list container that every shell then has to mirror again.

Design reason:

- native controls have different lifetime and virtualization behavior
- the most efficient bridge is usually the thinnest bridge that still preserves runtime semantics
- shell-specific adapters can exploit toolkit-native virtualization without polluting the shared runtime boundary

### 11.8 Range access is a first-class projection feature

Because large-library binding depends on lazy visible-range materialization, projections should not expose only single-item access.

They should also support efficient range-oriented access patterns such as:

- fetch `TrackId`s for `[start, end)`
- fetch lightweight row snapshots for `[start, end)`
- invalidate cached snapshots for a known set of IDs or ranges

Design reason:

- native list controls often request nearby rows together
- batching range fetches reduces repeated LMDB lookups, dictionary resolution, and adapter churn
- range-oriented APIs better match viewport-based virtualization than per-item callbacks alone

---

## 12. Frontend Shell Responsibilities

Frontend shells remain important, but their role is narrower and cleaner.

### 12.1 What shells own

Shells own:

- widget creation and destruction
- native layout systems
- visual design, styling, and animation
- toolkit-specific input handling
- shell navigation primitives
- shell-local transient state

### 12.2 What shells do not own

Shells do not own:

- canonical playback state
- library mutation rules
- selection semantics across logical views
- command routing policy
- domain-side invalidation behavior

### 12.3 Shell binding model

Each shell component should do only three things:

- read from one or more stores or projections
- dispatch typed commands
- react to published events when appropriate

This is the contract that makes GTK and WinUI3 peers rather than separate application implementations.

---

## 13. UI Customization Model

The runtime is designed to support flexible composition of built-in capabilities without opening the entire product to uncontrolled user-authored code.

### 13.1 Curated capability catalog

Customization should be based on a catalog of built-in capabilities such as:

- playback transport
- now playing summary
- output selector
- track list view
- inspector
- status and diagnostics area
- list navigation

Each capability declares:

- which stores it reads
- which commands it can dispatch
- which scope it depends on
- whether it is session-global or view-local

### 13.2 Repetition is normal

If a capability is session-global, it may be rendered multiple times.

Examples:

- two playback bars bound to the same `PlaybackStore`
- a compact now-playing widget plus a full playback panel
- multiple status renderers on different shell surfaces

### 13.3 View binding is explicit

If a capability is view-local, it must be bound to an explicit `ViewId` or to a clearly defined focus policy.

Examples:

- one inspector bound to the focused view
- two track lists each bound to different `ViewId`s
- one metadata panel bound to a pinned view, not the global focus

### 13.4 Quality control

The default product posture is to support composition of curated built-in capabilities, not arbitrary end-user scripting.

Design reason:

- arbitrary user-authored imperative code is hard to version, test, support, and explain
- it weakens product quality control and risks making app behavior unpredictable
- curated capabilities preserve flexibility while protecting the product from uncontrolled extension surfaces

### 13.5 Future hosted C++ runtime

If Aobus later requires a hosted C++ extension model, that hosted layer should sit **above** the same typed runtime contracts.

It should consume:

- typed commands
- typed events
- typed stores and projections

It should not replace the shared runtime with a stringly dynamic system.

---

## 14. Boundary Rules

The shared runtime boundary must follow strict type hygiene.

### 14.1 Allowed boundary shapes

Across the runtime boundary, payloads should use:

- `ao::*` value types
- STL containers and strings
- plain enums and structs
- typed IDs such as `TrackId`, `ListId`, and `ViewId`

### 14.2 Forbidden boundary leakage

The shared runtime must not expose UI toolkit types such as:

- `Gtk::Widget*`
- `Glib::ustring`
- `sigc::signal`
- `winrt::*`
- native toolkit handles or view objects

Design reason:

- toolkit types make the contract non-portable
- toolkit leakage would force GTK assumptions into WinUI3 and vice versa
- a clean boundary is the prerequisite for truly native multi-shell support

### 14.3 Widget anchors stay shell-local

Any concept like popover anchors, mouse coordinates relative to a widget, or toolkit-specific focus handles remains a shell concern.

The runtime should describe intent, not physical widget geometry.

---

## 15. Representative Flows

These examples show how the architecture behaves in practice.

### 15.1 Play from one of two playback bars

1. A user activates play in one playback bar.
2. That widget dispatches `PlaySelectionInFocusedView` or `ResumePlayback`.
3. The playback service handles the command.
4. The playback service updates playback sequence state and drives `Player`.
5. The playback store changes.
6. Both playback bars re-render from the same updated store.

Design reason:

- no playback bar is “the primary one”
- transport widgets are peers, not coordinators

### 15.2 Selection change in one of several track views

1. A shell track view changes selection.
2. The shell dispatches `SetViewSelection { viewId, selection }`.
3. The view registry updates that view-local state.
4. The focus store may update if that view became active.
5. An inspector bound to focused view re-renders.
6. Another inspector pinned to a different view remains unchanged.

Design reason:

- selection is local to a view
- focus is separate from selection
- several views can coexist without hidden coupling

### 15.3 Metadata edit from any editor surface

1. A track title is edited from a list cell, inspector field, or another curated editor.
2. The shell dispatches `UpdateTrackMetadata`.
3. The library mutation service performs the write.
4. The service invalidates affected projections and publishes `TracksMutated`.
5. All bound views update consistently from the same authoritative mutation path.

Design reason:

- every editor surface must converge on the same write behavior
- mutation side effects should not be duplicated in many shell-specific callbacks

### 15.4 Reveal currently playing track

1. The runtime determines that UI attention should move to the playing track.
2. The runtime publishes `RevealTrackRequested { trackId, preferredListId }`.
3. Each shell decides how to satisfy that request using its own navigation system.

Design reason:

- navigation mechanics are shell-specific
- navigation intent is runtime-level semantics

---

## 16. Design Rationale Summary

The architecture makes the following intentional choices.

### 16.1 Shared runtime over shell-centric coordinators

Reason:

- shell-centric coordinators assume one frontend and one canonical layout
- a shared runtime makes application semantics reusable across GTK and WinUI3

### 16.2 Typed commands instead of string topics

Reason:

- strong C++ typing catches mistakes earlier
- typed payloads are easier to evolve intentionally and review rigorously

### 16.3 Stores for current state

Reason:

- repeated widgets need current truth immediately
- state snapshots are clearer than reconstructing state from event history

### 16.4 Projections for large structured data

Reason:

- list membership and grouped collections need richer structure than a generic bus
- the existing domain already benefits from efficient observer-based propagation

### 16.5 Explicit view scope

Reason:

- multiple simultaneous track views require explicit local identity
- shell concepts such as “visible page” are not application semantics

### 16.6 Curated customization instead of arbitrary scripting

Reason:

- the product can remain flexible without sacrificing predictability and supportability
- built-in capability composition gives users power without turning the app into an unmanaged extension host

### 16.7 Semantic state over presentation strings

Reason:

- different shells should present the same meaning in native ways
- semantic contracts age better than UI text fragments

### 16.8 Control-context affinity over free-threaded stores

Reason:

- the app needs a clear serialized control plane
- direct cross-thread store mutation would blur ownership and violate the intended threading model

### 16.9 Explicit command completion over implicit success inference

Reason:

- optimistic UI needs a direct rollback signal
- callers should not infer failure from the absence of later events

### 16.10 Logical view identity over widget identity

Reason:

- a view may survive temporary widget teardown
- detached versus destroyed must be an intentional shell decision

### 16.11 Layered fault reporting over one universal error channel

Reason:

- initiating widgets, global observers, steady-state readers, and shell presenters need different error surfaces
- command completion, events, stores, and notifications solve different parts of the problem

---

## 17. Relationship To Other Design Docs

- [linux-gtk-ui-design.md](linux-gtk-ui-design.md) describes the current GTK shell's user-facing behavior and layout.
- [THREADING.md](THREADING.md) defines threading constraints that the shared runtime must continue to honor.
- [app-runtime-interface-draft.md](app-runtime-interface-draft.md) translates this architecture into a concrete C++-oriented interface draft.

This document sits above both of those. It defines the long-term application control architecture that future shells and built-in UI composition should rely on.
