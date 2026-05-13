# Runtime Design for Track Presentation Presets

## Purpose

This document defines runtime types and APIs for presentation presets. The skeletons are intentionally close to header shape so implementation can proceed phase by phase.

## File Ownership

Recommended runtime files:

```text
app/runtime/StateTypes.h                  # small persisted/state-facing enums and structs
app/runtime/ProjectionTypes.h             # projection snapshots
app/runtime/TrackPresentationPreset.h     # preset specs and registry API
app/runtime/TrackPresentationPreset.cpp   # built-in preset definitions
app/runtime/ViewService.h/.cpp            # active presentation per view
app/runtime/TrackListProjection.h/.cpp    # projection consumes group/sort and exposes snapshot
```

`TrackPresentationPreset.*` should replace the current group-derived `TrackListPresentation.*` once migration is complete.

## Runtime Semantic Field

Add a frontend-neutral field enum. This can live in `StateTypes.h` if it must be persisted, or in `TrackPresentationPreset.h` if persistence conversion is handled separately.

Recommended first location: `app/runtime/StateTypes.h`, because custom views and runtime view state will likely serialize these values.

```cpp
namespace ao::rt
{
  enum class TrackPresentationField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Duration,
    Tags,
  };
}
```

### Field string conversion

Add stable ids for config and tests.

```cpp
namespace ao::rt
{
  std::string_view trackPresentationFieldId(TrackPresentationField field);
  std::optional<TrackPresentationField> trackPresentationFieldFromId(std::string_view id);
}
```

Suggested ids:

| Field | ID |
| --- | --- |
| Title | `title` |
| Artist | `artist` |
| Album | `album` |
| AlbumArtist | `album-artist` |
| Genre | `genre` |
| Composer | `composer` |
| Work | `work` |
| Year | `year` |
| DiscNumber | `disc-number` |
| TrackNumber | `track-number` |
| Duration | `duration` |
| Tags | `tags` |

## Presentation Spec

Header skeleton for `app/runtime/TrackPresentationPreset.h`:

```cpp
#pragma once

#include "StateTypes.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr std::string_view kDefaultTrackPresentationId = "songs";

  struct TrackPresentationSpec final
  {
    std::string id{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackPresentationField> visibleFields{};
    std::vector<TrackPresentationField> redundantFields{};

    bool operator==(TrackPresentationSpec const&) const = default;
  };

  struct TrackPresentationPreset final
  {
    TrackPresentationSpec spec{};
    std::string_view label{};
    std::string_view description{};
  };

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets();
  TrackPresentationPreset const* builtinTrackPresentationPreset(std::string_view id);
  TrackPresentationSpec defaultTrackPresentationSpec();
  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec);
}
```

### Normalization rules

`normalizeTrackPresentationSpec()` should:

1. Convert empty id to `songs` only when used for active/default state.
2. Remove duplicate `visibleFields` while preserving first occurrence.
3. Remove duplicate `redundantFields` while preserving first occurrence.
4. Keep unknown fields impossible at type level; config parsing should drop unknown ids before constructing the enum.
5. Permit empty `sortBy` for custom drafts, but built-ins must always have explicit sort terms.

## Built-in Registry Implementation

Skeleton for `app/runtime/TrackPresentationPreset.cpp`:

```cpp
#include "TrackPresentationPreset.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace ao::rt
{
  namespace
  {
    using Field = TrackPresentationField;
    using Sort = TrackSortField;

    constexpr auto kBuiltinPresets = std::to_array<TrackPresentationPreset>({
      TrackPresentationPreset{
        .spec = TrackPresentationSpec{
          .id = "songs",
          .groupBy = TrackGroupKey::None,
          .sortBy = {
            TrackSortTerm{Sort::Artist, true},
            TrackSortTerm{Sort::Album, true},
            TrackSortTerm{Sort::DiscNumber, true},
            TrackSortTerm{Sort::TrackNumber, true},
            TrackSortTerm{Sort::Title, true},
          },
          .visibleFields = {Field::Title, Field::Artist, Field::Album, Field::Duration, Field::Tags},
          .redundantFields = {},
        },
        .label = "Songs",
        .description = "General-purpose song list.",
      },
      // Other built-ins follow the matrix in 01-architecture.md.
    });
  }
}
```

If `std::array` with `std::vector` members is inconvenient for constexpr initialization, use `const auto` inside a function-local static provider.

## Runtime View State

Recommended transitional shape in `StateTypes.h`:

```cpp
namespace ao::rt
{
  struct TrackListPresentationState final
  {
    std::string presentationId = std::string{kDefaultTrackPresentationId};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackPresentationField> visibleFields{};
    std::vector<TrackPresentationField> redundantFields{};

    bool operator==(TrackListPresentationState const&) const = default;
  };

  struct TrackListViewState final
  {
    ViewId id{};
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
    ListId listId{};
    std::string filterExpression{};
    TrackListPresentationState presentation{};
    std::vector<TrackId> selection{};
    std::uint64_t revision = 0;
  };

  struct TrackListViewConfig final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackListPresentationState presentation{};
    std::vector<TrackId> selection{};
  };
}
```

### Transitional compatibility

During early phases, it is acceptable to keep old `groupBy` and `sortBy` fields in `TrackListViewState` while adding `presentation`. Once all callers use `presentation`, remove the old fields.

If old fields remain temporarily, use a single helper to avoid divergence:

```cpp
namespace ao::rt
{
  TrackListPresentationState presentationStateFromSpec(TrackPresentationSpec const& spec);
  TrackPresentationSpec presentationSpecFromState(TrackListPresentationState const& state);
}
```

## Projection Snapshot

Update `app/runtime/ProjectionTypes.h`:

```cpp
namespace ao::rt
{
  struct TrackListPresentationSnapshot final
  {
    std::string presentationId{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> effectiveSortBy{};
    std::vector<TrackPresentationField> visibleFields{};
    std::vector<TrackPresentationField> redundantFields{};
    std::uint64_t revision = 0;
  };
}
```

`TrackListProjection` should store the full presentation snapshot/spec or enough fields to return this snapshot. It should only use `groupBy` and `sortBy` for ordering.

## TrackListProjection Skeleton

Current projection method:

```cpp
void setPresentation(TrackGroupKey groupBy, std::vector<TrackSortTerm> sortBy);
```

Target skeleton:

```cpp
class TrackListProjection final : public ITrackListProjection
{
public:
  void setPresentation(TrackPresentationSpec presentation);

  TrackListPresentationSnapshot presentation() const override;

private:
  // Impl stores:
  // TrackPresentationSpec presentation;
  // comparator built from presentation.sortBy
  // group sections built from presentation.groupBy
};
```

Implementation notes:

1. Keep comparator and load-mode code based on `presentation.sortBy` and `presentation.groupBy`.
2. Preserve the current fast path for direction-only sort reversals if possible.
3. Do not add any value lookup path for `visibleFields`.
4. Publish a reset delta when group/sort changes.
5. Presentation-only visible-field changes may still publish a reset at first; later this can become a lighter event if needed.

## ViewService API Skeleton

Update `app/runtime/ViewService.h`:

```cpp
class ViewService final
{
public:
  struct PresentationChanged final
  {
    ViewId viewId{};
    TrackPresentationSpec presentation{};
  };

  CreateTrackListViewReply createView(TrackListViewConfig const& initial, bool attached = true);

  void setPresentation(ViewId viewId, TrackPresentationSpec const& presentation);
  void setPresentation(ViewId viewId, std::string_view presentationId);

  // Compatibility during migration only.
  void setSort(ViewId viewId, std::vector<TrackSortTerm> const& sortBy);
  void setGrouping(ViewId viewId, TrackGroupKey groupBy);

  Subscription onPresentationChanged(std::move_only_function<void(PresentationChanged const&)> handler);
};
```

### ViewService implementation helper

Replace old `applyPresentation(ViewEntry&)` with spec-based helpers:

```cpp
namespace
{
  TrackPresentationSpec resolvePresentation(TrackListPresentationState const& state)
  {
    if (auto const* preset = builtinTrackPresentationPreset(state.presentationId))
    {
      return preset->spec;
    }

    return defaultTrackPresentationSpec();
  }

  void applyPresentation(ViewEntry& entry, TrackPresentationSpec const& spec)
  {
    entry.state.presentation = presentationStateFromSpec(spec);

    if (entry.projection)
    {
      if (auto* const trackListProj = dynamic_cast<TrackListProjection*>(entry.projection.get()))
      {
        trackListProj->setPresentation(spec);
      }
    }
  }
}
```

### Compatibility behavior

During migration:

- `setGrouping()` should create an equivalent built-in presentation where possible, or update the active spec's group and mark it custom/internal.
- `setSort()` should update the current spec's sort and mark it custom/internal.

After the GTK toolbar stops exposing group/sort directly, remove or narrow these APIs if no non-test callers remain.

## Runtime Events

`PresentationChanged` should be emitted when any presentation-level field changes:

- id
- group
- sort
- visible fields
- redundant fields

`SortChanged` and `GroupingChanged` can remain during migration for existing observers, but the target event for GTK track pages is `PresentationChanged`.

## Runtime Tests

Add tests for:

1. Built-in registry contains all expected ids.
2. Each built-in has exact group/sort/visible/redundant fields.
3. Default spec is `songs`.
4. Unknown id falls back to default in `ViewService::setPresentation(viewId, id)`.
5. `ViewService::setPresentation()` updates view state revision.
6. Projection receives group/sort from spec.
7. Projection snapshot includes visible/redundant fields.
8. Projection deltas remain range/track-id based and never contain row values.

## Deletion Target

Once migration is complete, delete the group-only presentation helper:

```cpp
presentationForGroup(TrackGroupKey groupBy)
```

No runtime code should derive sort from group-by alone after the preset registry is authoritative.
