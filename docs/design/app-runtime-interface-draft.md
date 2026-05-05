# Aobus Shared App Runtime Interface Draft

> **Audience:** Contributors designing or implementing the shared runtime, native shells, and projection adapters.
>
> **Purpose:** Translate the target architecture in [app-runtime-architecture.md](app-runtime-architecture.md) into a concrete C++-oriented interface draft.
>
> **Status:** This document is a draft API specification. It is intentionally more concrete than the architecture document, but it is still not an implementation plan.

---

## 1. Scope

This document defines the intended public interface shape for the shared app runtime.

It covers:

- runtime primitives
- control-context execution contracts
- command execution
- events and stores
- view lifecycle and scoped state
- projections for large track lists and detail views
- service interfaces
- shell-facing access patterns

It does not cover:

- migration from current GTK code
- ABI stability
- persistence or compatibility policy for view state
- implementation details of LMDB transaction reuse, caching policy, or threading internals

---

## 2. Draft Conventions

This draft assumes existing Aobus types where appropriate.

- `ao::Result<T>` and `ao::Error` from [include/ao/Error.h](file:///home/rocklee/dev/Aobus/include/ao/Error.h)
- `ao::TrackId`, `ao::ListId`, `ao::ResourceId`, and `ao::DictionaryId` from [include/ao/Type.h](file:///home/rocklee/dev/Aobus/include/ao/Type.h)
- audio identifiers and transport types from [include/ao/audio/Backend.h](file:///home/rocklee/dev/Aobus/include/ao/audio/Backend.h) and [include/ao/audio/Types.h](file:///home/rocklee/dev/Aobus/include/ao/audio/Types.h)

All interfaces in this document are assumed to live in `namespace ao::app` unless noted otherwise.

Code blocks are intended to be header-like and concrete, but small syntax adjustments may still be required during implementation.

---

## 3. Core Primitives

### 3.1 IDs and ranges

```cpp
namespace ao::app
{
  using ViewId = ao::utility::TaggedInteger<std::uint64_t, struct ViewIdTag>;
  using NotificationId = ao::utility::TaggedInteger<std::uint64_t, struct NotificationIdTag>;

  struct Range final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };
}
```

Notes:

- `ViewId` is session-local and never silently recycled within a session.
- `Range` is half-open by convention: `[start, start + count)`.

### 3.2 Subscription

The runtime uses RAII subscriptions for stores, events, and projections.

```cpp
namespace ao::app
{
  class Subscription final
  {
  public:
    Subscription() = default;
    explicit Subscription(std::move_only_function<void()> unsubscribe);

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    ~Subscription();

    void reset();
    explicit operator bool() const noexcept;

  private:
    std::move_only_function<void()> _unsubscribe;
  };
}
```

Semantics:

- destruction unsubscribes exactly once
- `reset()` unsubscribes early and makes the handle inert
- callbacks registered through a subscription are delivered only on the runtime control context unless a specific API explicitly documents otherwise

### 3.3 Control executor

The runtime needs a UI-neutral control-plane executor.

```cpp
namespace ao::app
{
  class IControlExecutor
  {
  public:
    virtual ~IControlExecutor() = default;

    virtual bool isCurrent() const noexcept = 0;
    virtual void dispatch(std::move_only_function<void()> task) = 0;
  };
}
```

Notes:

- this is the execution context described in [app-runtime-architecture.md](app-runtime-architecture.md)
- in GTK, the implementation will likely target the same thread currently served by [`IMainThreadDispatcher`](file:///home/rocklee/dev/Aobus/include/ao/utility/IMainThreadDispatcher.h)
- stores, event delivery, and command execution all happen on this context

### 3.4 Text strategy

The shared runtime does **not** own a UI-neutral text representation layer. Text ownership stays at the shell boundary:

- **Dictionary-backed fields** (artist, album, genre, composer, work, etc.): shell adapters resolve them via the existing `DictionaryStore::get(DictionaryId) -> std::string_view`. Copy into native string types (`Glib::ustring`, `hstring`) and cache locally keyed by `DictionaryId`.
- **Non-dictionary fields** (title): the shell adapter reads raw UTF-8 bytes directly from `TrackView` (the existing zero-copy LMDB parser). No intermediate runtime wrapper.
- **Sort keys**: the projection order index uses compact normalized sort keys (dictionary rank, or compact UTF-8 collation keys). These are never toolkit strings and never escape the projection implementation.

This means there is no intermediate text layer in the runtime interface. The projection delivers ordered `TrackId`s; the adapter owns all text materialization.

### 3.5 Fault snapshot

```cpp
namespace ao::app
{
  enum class FaultDomain : std::uint8_t
  {
    Playback,
    Output,
    Library,
    Import,
    Query,
    View,
    Generic,
  };

  struct FaultSnapshot final
  {
    FaultDomain domain = FaultDomain::Generic;
    ao::Error error{};
    bool transient = false;
  };
}
```

---

## 4. Generic Runtime Interfaces

### 4.1 Store delivery mode

```cpp
namespace ao::app
{
  enum class StoreDeliveryMode : std::uint8_t
  {
    ReplayCurrent,
    FutureOnly,
  };
}
```

### 4.2 Read-only store

```cpp
namespace ao::app
{
  template<class State>
  class IReadOnlyStore
  {
  public:
    virtual ~IReadOnlyStore() = default;

    virtual State snapshot() const = 0;
    virtual Subscription subscribe(std::move_only_function<void(State const&)> handler,
                                   StoreDeliveryMode mode = StoreDeliveryMode::ReplayCurrent) = 0;
  };
}
```

Semantics:

- `snapshot()` is a control-context API unless a store explicitly documents additional thread-safe snapshot behavior
- subscriber callbacks are delivered on the control context
- `ReplayCurrent` immediately emits the current value before future updates

### 4.3 Event source

```cpp
namespace ao::app
{
  class EventBus final
  {
  public:
    template<class Event>
    Subscription subscribe(std::move_only_function<void(Event const&)> handler);

    template<class Event>
    void publish(Event const& event);
  };
}
```

Semantics:

- shells subscribe through `EventBus`
- only runtime-owned services publish
- event delivery happens on the control context

### 4.4 Command delivery order guarantee

Within one command execution cycle on the control context, observable side effects follow a fixed order:

1. command handler executes
2. stores are mutated
3. projection deltas are published
4. events are published

A component subscribed to both a store and an event is guaranteed to receive the store update before the event. This order follows from the definitions in the architecture doc: stores represent current state (which changes first), events represent facts that became true (which are announced after).

### 4.5 Command bus

```cpp
namespace ao::app
{
  class CommandBus final
  {
  public:
    template<class Command>
    using Reply = typename Command::Reply;

    template<class Command>
    using Handler = std::move_only_function<ao::Result<Reply<Command>>(Command const&)>;

    template<class Command>
    ao::Result<Reply<Command>> execute(Command const& command);

    template<class Command>
    void registerHandler(Handler<Command> handler);
  };
}
```

Semantics:

- `execute()` requires `executor().isCurrent() == true`; calling from a non-control thread is a precondition violation
- one command type has exactly one registered handler per session
- execution is synchronous: the handler runs and the `Result<Reply>` is available before `execute()` returns
- if a command handler itself calls `execute()` with another command, execution nests on the call stack (re-entrant); handlers that intentionally want deferred execution post through `IControlExecutor::dispatch` explicitly
- cross-thread callers marshal onto the control context through `IControlExecutor::dispatch` before calling `execute()`

---

## 5. Shared Runtime State Types

### 5.1 Playback state

```cpp
namespace ao::app
{
  struct OutputProfileSnapshot final
  {
    ao::audio::ProfileId id{};
    std::string name{};
    std::string description{};
  };

  struct OutputDeviceSnapshot final
  {
    ao::audio::DeviceId id{};
    std::string displayName{};
    std::string description{};
    bool isDefault = false;
    ao::audio::BackendId backendId{};
    ao::audio::DeviceCapabilities capabilities{};
  };

  struct OutputBackendSnapshot final
  {
    ao::audio::BackendId id{};
    std::string name{};
    std::string description{};
    std::string iconName{};
    std::vector<OutputProfileSnapshot> supportedProfiles{};
    std::vector<OutputDeviceSnapshot> devices{};
  };

  struct OutputSelection final
  {
    ao::audio::BackendId backendId{};
    ao::audio::DeviceId deviceId{};
    ao::audio::ProfileId profileId{};

    bool operator==(OutputSelection const&) const = default;
  };

  struct PlaybackState final
  {
    ao::audio::Transport transport = ao::audio::Transport::Idle;
    ao::TrackId trackId{};
    ao::ListId sourceListId{};
    std::uint32_t positionMs = 0;
    std::uint32_t durationMs = 0;
    float volume = 1.0f;
    bool muted = false;
    bool volumeAvailable = false;
    bool ready = false;

    OutputSelection selectedOutput{};
    std::vector<OutputBackendSnapshot> availableOutputs{};
    ao::audio::flow::Graph flow{};
    ao::audio::Quality quality = ao::audio::Quality::Unknown;
    std::optional<FaultSnapshot> optFault{};
    std::uint64_t revision = 0;
  };
}
```

### 5.2 Focus and notifications

```cpp
namespace ao::app
{
  struct FocusState final
  {
    ViewId focusedView{};
    std::uint64_t revision = 0;
  };

  enum class NotificationSeverity : std::uint8_t
  {
    Info,
    Warning,
    Error,
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    bool sticky = false;
    std::optional<std::chrono::milliseconds> timeout{};
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
    std::uint64_t revision = 0;
  };
}
```

### 5.3 View state

The runtime must not depend on GTK enums for grouping and sorting, so it defines neutral equivalents.

```cpp
namespace ao::app
{
  enum class TrackGroupKey : std::uint8_t
  {
    None,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
  };

  enum class TrackSortField : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Title,
    Duration,
  };

  struct TrackSortTerm final
  {
    TrackSortField field = TrackSortField::Title;
    bool ascending = true;
  };

  enum class ViewLifecycleState : std::uint8_t
  {
    Attached,
    Detached,
    Destroyed,
  };

  enum class ViewKind : std::uint8_t
  {
    TrackList,
    Inspector,
    Auxiliary,
  };

  struct TrackListViewState final
  {
    ViewId id{};
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
    ao::ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<ao::TrackId> selection{};
    std::uint64_t revision = 0;
  };

  struct TrackListViewConfig final
  {
    ao::ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<ao::TrackId> selection{};
  };

  struct ViewRecord final
  {
    ViewId id{};
    ViewKind kind = ViewKind::TrackList;
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
  };
}
```

---

## 6. Command Types

The following command set is concrete enough to drive the current GTK shell and a future WinUI3 shell.

### 6.1 Playback commands

```cpp
namespace ao::app
{
  struct PlayTrack final
  {
    using Reply = void;

    ao::TrackId trackId{};
    ao::ListId sourceListId{};
  };

  struct PlaySelectionInView final
  {
    using Reply = ao::TrackId;

    ViewId viewId{};
  };

  struct PlaySelectionInFocusedView final
  {
    using Reply = ao::TrackId;
  };

  struct PausePlayback final
  {
    using Reply = void;
  };

  struct ResumePlayback final
  {
    using Reply = void;
  };

  struct StopPlayback final
  {
    using Reply = void;
  };

  struct SeekPlayback final
  {
    using Reply = void;

    std::uint32_t positionMs = 0;
  };

  struct SetPlaybackOutput final
  {
    using Reply = void;

    ao::audio::BackendId backendId{};
    ao::audio::DeviceId deviceId{};
    ao::audio::ProfileId profileId{};
  };

  struct SetPlaybackVolume final
  {
    using Reply = void;

    float volume = 1.0f;
  };

  struct SetPlaybackMuted final
  {
    using Reply = void;

    bool muted = false;
  };
}
```

### 6.2 Library mutation commands

```cpp
namespace ao::app
{
  struct MetadataPatch final
  {
    std::optional<std::string> title{};
    std::optional<std::string> artist{};
    std::optional<std::string> album{};
    std::optional<std::string> genre{};
    std::optional<std::string> composer{};
    std::optional<std::string> work{};
  };

  struct UpdateTrackMetadataReply final
  {
    std::vector<ao::TrackId> mutatedIds{};
  };

  struct UpdateTrackMetadata final
  {
    using Reply = UpdateTrackMetadataReply;

    std::vector<ao::TrackId> trackIds{};
    MetadataPatch patch{};
  };

  struct EditTrackTagsReply final
  {
    std::vector<ao::TrackId> mutatedIds{};
  };

  struct EditTrackTags final
  {
    using Reply = EditTrackTagsReply;

    std::vector<ao::TrackId> trackIds{};
    std::vector<std::string> tagsToAdd{};
    std::vector<std::string> tagsToRemove{};
  };

  struct ImportFilesReply final
  {
    std::size_t importedTrackCount = 0;
  };

  struct ImportFiles final
  {
    using Reply = ImportFilesReply;

    std::vector<std::filesystem::path> paths{};
  };
}
```

### 6.3 View and focus commands

```cpp
namespace ao::app
{
  struct CreateTrackListViewReply final
  {
    ViewId viewId{};
  };

  struct CreateTrackListView final
  {
    using Reply = CreateTrackListViewReply;

    TrackListViewConfig initial{};
    bool attached = true;
  };

  struct AttachView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct DetachView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct DestroyView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct OpenListInView final
  {
    using Reply = void;

    ViewId viewId{};
    ao::ListId listId{};
  };

  struct SetViewFilter final
  {
    using Reply = void;

    ViewId viewId{};
    std::string filterExpression{};
  };

  struct SetViewGrouping final
  {
    using Reply = void;

    ViewId viewId{};
    TrackGroupKey groupBy = TrackGroupKey::None;
  };

  struct SetViewSort final
  {
    using Reply = void;

    ViewId viewId{};
    std::vector<TrackSortTerm> sortBy{};
  };

  struct SetViewSelection final
  {
    using Reply = void;

    ViewId viewId{};
    std::vector<ao::TrackId> selection{};
  };

  struct SetFocusedView final
  {
    using Reply = void;

    ViewId viewId{};
  };
}
```

---

## 7. Event Types

Events report facts that became true after commands or internal state transitions.

### 7.1 Playback events

```cpp
namespace ao::app
{
  struct PlaybackTransportChanged final
  {
    ao::audio::Transport transport = ao::audio::Transport::Idle;
  };

  struct NowPlayingTrackChanged final
  {
    ao::TrackId trackId{};
    ao::ListId sourceListId{};
  };

  struct PlaybackOutputChanged final
  {
    OutputSelection selection{};
  };

  struct PlaybackFaultTransition final
  {
    std::optional<FaultSnapshot> optFault{};
  };
}
```

### 7.2 Library events

```cpp
namespace ao::app
{
  struct TracksMutated final
  {
    std::vector<ao::TrackId> trackIds{};
  };

  struct ListsMutated final
  {
    std::vector<ao::ListId> listIds{};
  };

  struct LibraryImportCompleted final
  {
    std::size_t importedTrackCount = 0;
  };

  struct ImportProgressUpdated final
  {
    double fraction = 0.0;
    std::string message{};
  };
}
```

### 7.3 View events

```cpp
namespace ao::app
{
  struct FocusedViewChanged final
  {
    ViewId viewId{};
  };

  struct ViewDestroyed final
  {
    ViewId viewId{};
  };

  struct RevealTrackRequested final
  {
    ao::TrackId trackId{};
    ao::ListId preferredListId{};
    ViewId preferredViewId{};
  };
}
```

### 7.4 Notification events

```cpp
namespace ao::app
{
  struct NotificationPosted final
  {
    NotificationId id{};
  };

  struct NotificationDismissed final
  {
    NotificationId id{};
  };
}
```

---

## 8. Text Strategy

The shared runtime does not introduce an intermediate text layer. Dictionary-backed metadata (artist, album, genre, etc.) is resolved via the existing `DictionaryStore::get(DictionaryId)`. Non-dictionary text (title) is read directly from `TrackView`. All native string caching (`Glib::ustring`, `hstring`) is a shell-adapter concern.

---

## 9. Projections

### 9.1 Track list projection deltas

```cpp
namespace ao::app
{
  struct ProjectionReset final
  {
    std::uint64_t revision = 0;
  };

  struct ProjectionInsertRange final
  {
    Range range{};
  };

  struct ProjectionRemoveRange final
  {
    Range range{};
  };

  struct ProjectionUpdateRange final
  {
    Range range{};
  };

  using TrackListProjectionDelta = std::variant<ProjectionReset,
                                                ProjectionInsertRange,
                                                ProjectionRemoveRange,
                                                ProjectionUpdateRange>;

  struct TrackListProjectionDeltaBatch final
  {
    std::uint64_t revision = 0;
    std::vector<TrackListProjectionDelta> deltas{};
  };
}
```

### 9.2 Track list projection interface

```cpp
namespace ao::app
{
  class ITrackListProjection
  {
  public:
    virtual ~ITrackListProjection() = default;

    virtual ViewId viewId() const noexcept = 0;
    virtual std::uint64_t revision() const noexcept = 0;

    virtual std::size_t size() const noexcept = 0;
    virtual ao::TrackId trackIdAt(std::size_t index) const = 0;

    virtual Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) = 0;
  };
}
```

Semantics:

- the projection owns ordered identity, not toolkit row objects
- **sort order index**: a sorted projection eagerly maintains a compact order index for all members. Sort keys are numeric (year, disc/track number, duration), dictionary ranks, or compact UTF-8 collation keys — never toolkit strings. Sort key storage is the only eager per-item data the projection owns beyond `TrackId` identity.
- sorting, grouping, and filtering changes trigger a `ProjectionReset` — the shell adapter discards its local model and rebuilds
- row snapshots are lazy, bounded, and field-selective; text is not included
- delta delivery uses ranges to match native list model expectations
- **reset-on-subscribe**: before `subscribe()` returns, `handler` is called synchronously with a single `ProjectionReset` delta. This establishes the current projection size and revision without forcing a full-data copy. Callers may then query `size()` and `trackIdAt()` without a gap between initial state capture and delta subscription. Subsequent deltas are delivered on the control context.

### 9.5 Detail projection

```cpp
namespace ao::app
{
  enum class SelectionKind : std::uint8_t
  {
    None,
    Single,
    Multiple,
  };

  template<class T>
  struct AggregateValue final
  {
    std::optional<T> value{};
    bool mixed = false;
  };

  struct AudioPropertySnapshot final
  {
    AggregateValue<std::uint16_t> codecId{};
    AggregateValue<std::uint32_t> sampleRate{};
    AggregateValue<std::uint8_t> channels{};
    AggregateValue<std::uint8_t> bitDepth{};
    AggregateValue<std::uint32_t> durationMs{};
  };

  struct TrackDetailSnapshot final
  {
    SelectionKind selectionKind = SelectionKind::None;
    std::vector<ao::TrackId> trackIds{};
    std::uint64_t revision = 0;

    ao::ResourceId singleCoverArtId{};
    AudioPropertySnapshot audio{};
    std::vector<ao::DictionaryId> commonTagIds{};
  };

  struct FocusedViewTarget final
  {
  };

  struct ExplicitViewTarget final
  {
    ViewId viewId{};
  };

  struct ExplicitSelectionTarget final
  {
    std::vector<ao::TrackId> trackIds{};
  };

  using DetailTarget = std::variant<FocusedViewTarget, ExplicitViewTarget, ExplicitSelectionTarget>;

  class ITrackDetailProjection
  {
  public:
    virtual ~ITrackDetailProjection() = default;

    virtual TrackDetailSnapshot snapshot() const = 0;
    virtual Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler,
                                   StoreDeliveryMode mode = StoreDeliveryMode::ReplayCurrent) = 0;
  };
}
```

---

## 10. View Registry

The `ViewRegistry` owns logical view lifetime and state. It is read-only from the shell's perspective — all mutations go through commands.

```cpp
namespace ao::app
{
  class ViewRegistry
  {
  public:
    virtual ~ViewRegistry() = default;

    virtual std::vector<ViewRecord> listViews() const = 0;

    virtual IReadOnlyStore<TrackListViewState>& trackListState(ViewId viewId) = 0;
    virtual std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId) = 0;
  };
}
```

Shells create and manage views exclusively through the command bus:

```cpp
auto result = session.commands().execute(CreateTrackListView{
    .initial = {.listId = someListId},
    .attached = true,
});
auto viewId = result->viewId;
```

Semantics:

- `CreateTrackListView` creates the logical view and its projection
- `AttachView` / `DetachView` transition between attached and detached without destroying state
- `DestroyView` retires the view and invalidates future use of its `ViewId`
- shells decide whether a close action maps to `DetachView` or `DestroyView`

---

## 11. Services

### 11.1 Playback service

`PlaybackService` owns playback coordination. It registers itself as the command handler for all playback commands during `AppSession` construction. It is **not** exposed to shells directly — shells drive playback exclusively through `commands().execute(...)`.

Its only shell-visible surface is the read-only playback store:

```cpp
namespace ao::app
{
  class PlaybackService
  {
  public:
    virtual ~PlaybackService() = default;

    virtual IReadOnlyStore<PlaybackState>& state() = 0;
  };
}
```

### 11.2 Library mutation service

`LibraryMutationService` owns write-side library mutations. It registers itself as the command handler for `UpdateTrackMetadata`, `EditTrackTags`, and `ImportFiles` during `AppSession` construction. It is **not** exposed to shells directly.

### 11.3 Library query service

```cpp
namespace ao::app
{
  class LibraryQueryService
  {
  public:
    virtual ~LibraryQueryService() = default;

    virtual std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId) = 0;
    virtual std::shared_ptr<ITrackDetailProjection> detailProjection(DetailTarget target) = 0;
  };
}
```

### 11.4 Notification service

```cpp
namespace ao::app
{
  class NotificationService
  {
  public:
    virtual ~NotificationService() = default;

    virtual IReadOnlyStore<NotificationFeedState>& feed() = 0;

    virtual NotificationId post(NotificationSeverity severity,
                                std::string message,
                                bool sticky = false,
                                std::optional<std::chrono::milliseconds> timeout = std::nullopt) = 0;

    virtual void dismiss(NotificationId id) = 0;
    virtual void dismissAll() = 0;
  };
}
```

---

## 12. `AppSession` Surface

This is the shell-facing root object.

```cpp
namespace ao::app
{
  struct AppSessionDependencies final
  {
    std::shared_ptr<IControlExecutor> executor{};
    std::filesystem::path libraryRoot{};
  };

  class AppSession final
  {
  public:
    explicit AppSession(AppSessionDependencies dependencies);
    ~AppSession();

    AppSession(AppSession const&) = delete;
    AppSession& operator=(AppSession const&) = delete;
    AppSession(AppSession&&) = delete;
    AppSession& operator=(AppSession&&) = delete;

    IControlExecutor& executor() noexcept;

    CommandBus& commands() noexcept;
    EventBus& events() noexcept;

    IReadOnlyStore<PlaybackState>& playback() noexcept;
    IReadOnlyStore<FocusState>& focus() noexcept;
    IReadOnlyStore<NotificationFeedState>& notifications() noexcept;

    ViewRegistry& views() noexcept;
    LibraryQueryService& queries() noexcept;
    NotificationService& notificationService() noexcept;
  };
}
```

Expected usage:

- shell widgets execute commands through `commands()`
- shell widgets read state through read-only stores
- shell list controls bind to projections from `queries()` or `views()`

`PlaybackService` and `LibraryMutationService` are internal — they register handlers on `CommandBus` at construction time and are invisible to shells. Only `queries()` (read-side projections), `views()` (view state and projections), and `notificationService()` (user-facing messages) remain shell-accessible beyond stores and the two buses.

---

## 13. Shell Adapter Contracts

The shared runtime stops at projections and numeric row snapshots. Shells add toolkit-native adapters that own all text materialization.

### 13.1 Track list adapter contract

The shell adapter for a native list control should follow this model:

```cpp
namespace ao::app
{
  class ITrackListShellAdapter
  {
  public:
    virtual ~ITrackListShellAdapter() = default;

    virtual void bind(std::shared_ptr<ITrackListProjection> projection) = 0;

    virtual void unbind() = 0;
  };
}
```

Behavioral contract:

- on `bind()`, subscribe to projection deltas (first `reset` arrives synchronously)
- translate `TrackListProjectionDeltaBatch` directly into native list notifications
- resolve dictionary-backed text via `DictionaryStore::get()`, non-dictionary text via `TrackView`
- keep only a bounded cache of native row wrappers and native strings

### 13.2 Text access pattern

The runtime does not own text. Shell adapters are responsible for all text materialization.

The intended pattern:

- **dictionary-backed fields** (artist, album, genre, etc.): `DictionaryStore::get(dictionaryId)` returns `std::string_view`. Copy into native string and cache keyed by `DictionaryId`.
- **non-dictionary fields** (title): read raw UTF-8 bytes from `TrackView` via the library layer. Cache keyed by `(TrackId, rowRevision)`.
- **sort and grouping**: the projection order index already provides ordering; the adapter never needs to sort by text.
- cache lifetime is shell-local and may be bounded by viewport or LRU policy.

This preserves UI neutrality — the runtime interface contains no `Glib::ustring`, no `hstring`, and no intermediate text wrapper.

---

## 14. Example Usage Sketch

### 14.1 Title edit with rollback

```cpp
auto result = session.commands().execute(ao::app::UpdateTrackMetadata{
    .trackIds = {trackId},
    .patch = ao::app::MetadataPatch{.title = newTitle},
});

if (!result)
{
    cachedRowHandle->restoreTitle(oldTitle);
}
```

### 14.2 GTK list binding sketch

```cpp
auto projection = session.views().trackListProjection(viewId);
_adapter.bind(projection);
```

The GTK adapter then:

- subscribes to projection deltas (the first `reset` arrives synchronously during `subscribe()`)
- maps `insert_range`, `remove_range`, `update_range`, and `reset` into `Gio::ListModel` notifications
- calls `trackIdAt()` for visible indices, reads row data from `TrackView`
- resolves dictionary text via `DictionaryStore::get()`, caches as `Glib::ustring` locally

---

## 15. Relationship To The Architecture Doc

This document is the concrete API counterpart to [app-runtime-architecture.md](app-runtime-architecture.md).

- the architecture doc defines motivation, boundaries, and design reasons
- this document defines concrete interface shapes implied by those architectural choices

If the two ever disagree, the architecture doc should be treated as the higher-level source of truth and both documents should be reconciled together.
