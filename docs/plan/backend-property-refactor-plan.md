# Audio Control Pipeline Refactor: Property-based Backend Controls (V3)

## 1. Goal

The goal of this refactor is not ABI preservation. The goal is defensive design:

- keep `IBackend` from turning into a long list of one-off control virtuals
- keep lifecycle and state-machine operations explicit
- avoid replacing "too many virtual methods" with an unbounded "everything bag"
- separate runtime controls from output selection, capabilities, and backend identity

The property system should solve a narrow problem well: cross-backend runtime controls such as `Volume` and `Muted`.

---

## 2. Design Principles

### 2.1 Keep Lifecycle Explicit

Operations that drive the backend state machine remain dedicated virtual methods:

- `open`
- `start`
- `pause`
- `resume`
- `flush`
- `drain`
- `stop`
- `close`
- `reset`

These operations have sequencing rules and side effects that are clearer and safer as named methods.

### 2.2 Use Properties Only for Runtime Controls

Properties are for values that tune the behavior of an already selected backend instance at runtime.

Initial scope:

- `Volume`
- `Muted`

This is the smallest useful slice that reduces interface bloat without collapsing unrelated concepts into a generic bag.

### 2.3 Do Not Turn Output Selection into a Property

Shared vs exclusive mode is currently part of output/profile selection, not a normal live control.

The following stay outside the property system:

- `BackendId`
- `ProfileId`
- device selection
- route identity
- negotiated stream format

If a change usually requires choosing a different backend instance, reopening a stream, or changing routing semantics, it should not be modeled as a phase-1 property.

### 2.4 Capabilities Are Not the Same as Support

A simple `supportsProperty(id)` boolean is too weak for defensive design.

We need to distinguish between:

- a property that exists in principle
- a property that is readable
- a property that is writable
- a property that is currently available on this backend/profile/device

That is especially important for cases like volume control, where availability can depend on the active backend mode or whether a mixer endpoint exists.

---

## 3. Control Taxonomy

| Category | Examples | Representation | Reason |
| --- | --- | --- | --- |
| Lifecycle and state transitions | `open`, `start`, `pause`, `resume`, `flush`, `drain`, `stop`, `close`, `reset` | Explicit virtual methods | These drive the backend state machine and have unique sequencing semantics. |
| Runtime controls | `Volume`, `Muted` | Typed property API | Prevents one virtual method pair per control. |
| Dynamic capability/status | volume availability | Property metadata query | Availability is runtime-dependent and should not be inferred from enum presence alone. |
| Output identity and selection | `BackendId`, `ProfileId`, device | Explicit provider/controller selection | These define which backend instance is active rather than how it is tuned. |

---

## 4. Proposed Property API

### 4.1 Typed Property Definitions

```cpp
// include/ao/audio/Property.h

enum class PropertyId
{
  Volume,
  Muted,
};

using PropertyValue = std::variant<float, bool>;

struct PropertyInfo final
{
  bool canRead = false;
  bool canWrite = false;
  bool isAvailable = false;
  bool emitsChangeNotifications = false;
};

template <typename T, PropertyId Id>
struct TypedProperty final
{
  using ValueType = T;
  static constexpr PropertyId id = Id;
};

namespace props
{
  inline constexpr auto Volume = TypedProperty<float, PropertyId::Volume>{};
  inline constexpr auto Muted = TypedProperty<bool, PropertyId::Muted>{};
}
```

### 4.2 `IBackend` Shape

```cpp
class IBackend
{
public:
  virtual ~IBackend() = default;

  // Lifecycle / transport control
  virtual Result<> open(Format const& format, RenderCallbacks callbacks) = 0;
  virtual void reset() = 0;
  virtual void start() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void flush() = 0;
  virtual void drain() = 0;
  virtual void stop() = 0;
  virtual void close() = 0;

  // Runtime control surface
  virtual Result<> setProperty(PropertyId id, PropertyValue const& value) = 0;
  virtual Result<PropertyValue> getProperty(PropertyId id) const = 0;
  virtual PropertyInfo queryProperty(PropertyId id) const noexcept = 0;

  template <typename T, PropertyId Id>
  Result<> set(TypedProperty<T, Id>, T value)
  {
    return setProperty(Id, PropertyValue{value});
  }

  template <typename T, PropertyId Id>
  Result<T> get(TypedProperty<T, Id>) const
  {
    auto result = getProperty(Id);
    if (!result)
    {
      return std::unexpected(result.error());
    }

    return std::get<T>(*result);
  }

  virtual BackendId backendId() const noexcept = 0;
  virtual ProfileId profileId() const noexcept = 0;
};
```

Notes:

- `profileId()` stays explicit because profile selection is still part of output selection.
- `queryProperty()` replaces `supportsProperty()` so callers can reason about dynamic availability.
- The property API is intentionally narrow in phase 1.

---

## 5. Callback Model

Backends still need a way to report external changes such as system volume updates. The callback should be invalidation-based rather than payload-based.

```cpp
struct RenderCallbacks final
{
  // ... existing callbacks ...
  void (*onPropertyChanged)(void* userData, PropertyId id) noexcept = nullptr;
};
```

Rationale:

- the backend only reports that a property changed
- `Engine` re-reads the current backend state on its own thread path
- we avoid passing `PropertyValue` payloads across backend callback boundaries
- late notifications after a backend switch become harmless refreshes instead of stale writes from an old backend instance

This is simpler and more robust than pushing `PropertyValue const&` through the callback surface.

---

## 6. Engine Model

### 6.1 Keep Hot Fields Explicit

`Engine::Status` and `Player::Status` should keep direct fields for the hot UI path:

- `volume`
- `muted`
- `volumeAvailable`

We do not need a general property bag in `Player::Status` yet. There is no evidence that the current UI needs that complexity.

### 6.2 Internal Behavior

`Engine` remains the place that translates property operations into UI-visible hot state.

Phase-1 behavior:

- `Engine::setVolume()` calls `backend->set(props::Volume, value)`
- `Engine::setMuted()` calls `backend->set(props::Muted, value)`
- `Engine::handlePropertyChanged(id)` re-reads the changed property from the current backend
- `Engine` uses `queryProperty(PropertyId::Volume).isAvailable` to drive `volumeAvailable`

### 6.3 Backend Switch Semantics

On backend switch, `Engine` should refresh its hot snapshot from the new backend.

Phase-1 rule:

- do not blindly replay old volume/mute values into the new backend
- adopt the new backend's observed property state

If we later want cross-output persistence, that should be an explicit policy decision in a separate follow-up, because it changes user-visible semantics.

### 6.4 Software Fallback Is Out of Scope

This refactor should not introduce software gain or mute fallback in the DSP path.

If we ever add fallback later, we should first introduce an explicit distinction between:

- desired value
- backend-reported value
- effective value applied in the data path

Without that split, property state becomes ambiguous.

---

## 7. What Is Explicitly Not a Property

### 7.1 `ExclusiveMode`

`ExclusiveMode` is not part of phase 1.

Reason:

- it behaves more like output/profile selection than a normal live control
- it can affect stream creation, routing, and capability availability
- the current system already models this through `ProfileId` and provider selection

For now, shared vs exclusive remains controlled by output selection.

### 7.2 `LatencyHint`

Also excluded from phase 1.

If a control requires reopen semantics, affects stream negotiation, or is not meaningfully live, it may belong in backend open options or provider/profile selection rather than the runtime property API.

### 7.3 Identity and Route Data

The following are not properties:

- `backendId()`
- `profileId()`
- device identity
- route anchor
- negotiated format

These are part of control-plane identity and topology, not runtime tuning knobs.

---

## 8. Admission Rule for New Properties

A new control should enter the property system only if all of the following are true:

1. It is meaningful on an already created backend instance.
2. It does not choose a different backend, device, or profile.
3. It does not require its own lifecycle contract beyond `set/query/notify`.
4. Its availability can be queried cleanly at runtime.
5. There is a real caller for it, not just hypothetical future extensibility.

If a candidate fails any of these checks, it should stay as:

- explicit output selection state
- open-time configuration
- backend-specific policy
- or a dedicated method if it truly has unique semantics

This rule is what keeps the property system from turning into another form of sprawl.

---

## 9. Migration Plan

1. Add `Property.h` with `PropertyId`, `PropertyValue`, `PropertyInfo`, and `TypedProperty`.
2. Extend `RenderCallbacks` with `onPropertyChanged(void*, PropertyId)`.
3. Add `setProperty`, `getProperty`, and `queryProperty` to `IBackend`.
4. Implement `Volume` and `Muted` in `NullBackend`, `PipeWireBackend`, and `AlsaExclusiveBackend`.
5. Update `Engine` to use the property API internally while keeping its public `setVolume`, `getVolume`, `setMuted`, `isMuted`, and `isVolumeAvailable` convenience methods.
6. Update `Player` and UI code only as needed to keep consuming the same hot fields.
7. Remove backend-specific `setVolume`, `getVolume`, `setMuted`, `isMuted`, and `isVolumeAvailable` virtuals once the property path is complete.
8. Evaluate future property candidates only through the admission rule above.

---

## 10. Validation Checklist

- adding a new runtime control no longer requires a dedicated pair of backend virtual methods by default
- volume and mute behavior seen by the UI remains unchanged
- backend switching refreshes from the new backend instead of applying stale payloads from the old one
- PipeWire shared/exclusive routing remains controlled by profile selection
- `IBackend` remains explicit for lifecycle/state-machine operations and compact for ordinary runtime controls

---

## 11. Summary

The property system should be a narrow tool, not a universal abstraction.

This refactor becomes safer and more maintainable if we:

- keep lifecycle explicit
- property-ize only true runtime controls
- keep profile/output selection out of the property bag
- use invalidation-based property notifications
- grow the system only when a new control passes a clear admission rule

That gives us the main win we want: no slide into dozens of backend virtual functions, without creating a second, harder-to-reason-about control surface.
