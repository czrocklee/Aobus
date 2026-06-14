// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldReader.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    SelectionKind selectionKindFromCount(std::size_t count)
    {
      if (count == 0)
      {
        return SelectionKind::None;
      }

      if (count == 1)
      {
        return SelectionKind::Single;
      }

      return SelectionKind::Multiple;
    }

    template<typename T>
    AggregateValue<T> aggregate(std::vector<T> const& values)
    {
      if (values.empty())
      {
        return {};
      }

      auto const& firstValue = values.front();

      for (std::size_t i = 1; i < values.size(); ++i)
      {
        if (values[i] != firstValue)
        {
          return {.mixed = true};
        }
      }

      return {.optValue = firstValue};
    }

    struct CustomAggregationState final
    {
      std::size_t presentCount = 0;
      std::optional<std::string> optFirstValue{};
      bool mixed = false;
    };

    void aggregateFields(library::TrackView const& view,
                         library::DictionaryStore const& dict,
                         library::FileManifestStore::Reader const* manifestReader,
                         std::array<std::vector<TrackFieldRawValue>, kTrackFieldCount>& fieldValues)
    {
      for (auto const& def : trackFieldDefinitions())
      {
        if (def.synthetic || def.category == TrackFieldCategory::Tag)
        {
          continue;
        }

        auto val = readTrackFieldRawValue(def.field, view, dict, manifestReader);
        trackFieldArrayAt(fieldValues, def.field).push_back(std::move(val));
      }
    }

    void aggregateCustom(library::TrackView const& view,
                         library::DictionaryStore const& dict,
                         std::map<std::string, CustomAggregationState>& customMap)
    {
      for (auto const& [dictId, value] : view.customMetadata())
      {
        auto const key = std::string{dict.getOrDefault(dictId)};

        if (key.empty())
        {
          continue;
        }

        auto& state = customMap[key];
        state.presentCount++;

        if (!state.optFirstValue)
        {
          state.optFirstValue = std::string{value};
        }
        else if (value != *state.optFirstValue)
        {
          state.mixed = true;
        }
      }
    }
  }

  struct TrackDetailProjection::Impl final
  {
    DetailTarget target;
    ViewService& views;
    library::MusicLibrary& library;
    WorkspaceService& workspace;
    LibraryMutationService& mutation;

    TrackDetailSnapshot cachedSnapshot;
    std::uint64_t revision = 0;
    std::vector<std::move_only_function<void(TrackDetailSnapshot const&)>> subscribers;
    Subscription focusSub;
    Subscription selectionSub;
    Subscription tracksMutatedSub;
    ViewId trackedViewId = rt::kInvalidViewId;

    Impl(DetailTarget target,
         ViewService& views,
         library::MusicLibrary& library,
         WorkspaceService& workspace,
         LibraryMutationService& mutation)
      : target{std::move(target)}, views{views}, library{library}, workspace{workspace}, mutation{mutation}
    {
    }
  };

  TrackDetailProjection::TrackDetailProjection(DetailTarget target,
                                               ViewService& views,
                                               library::MusicLibrary& library,
                                               WorkspaceService& workspace,
                                               LibraryMutationService& mutation)
    : _implPtr{std::make_unique<Impl>(std::move(target), views, library, workspace, mutation)}
  {
    std::visit(
      [this](auto const& target)
      {
        using T = std::decay_t<decltype(target)>;

        if constexpr (std::is_same_v<T, FocusedViewTarget>)
        {
          auto const layout = _implPtr->workspace.layoutState();
          _implPtr->trackedViewId = layout.activeViewId;

          if (_implPtr->trackedViewId != rt::kInvalidViewId)
          {
            auto const state = _implPtr->views.trackListState(_implPtr->trackedViewId);
            _implPtr->cachedSnapshot = buildSnapshot(state.selection);
          }

          _implPtr->focusSub = _implPtr->workspace.onFocusedViewChanged(
            [this](ViewId viewId)
            {
              _implPtr->trackedViewId = viewId;

              if (viewId == rt::kInvalidViewId)
              {
                _implPtr->cachedSnapshot = {};
                publishSnapshot();
                return;
              }

              auto const state = _implPtr->views.trackListState(viewId);
              _implPtr->cachedSnapshot = buildSnapshot(state.selection);
              publishSnapshot();
            });
        }
        else if constexpr (std::is_same_v<T, ExplicitViewTarget>)
        {
          _implPtr->trackedViewId = target.viewId;
          auto const state = _implPtr->views.trackListState(target.viewId);
          _implPtr->cachedSnapshot = buildSnapshot(state.selection);
        }
        else if constexpr (std::is_same_v<T, ExplicitSelectionTarget>)
        {
          _implPtr->cachedSnapshot = buildSnapshot(target.trackIds);
        }
      },
      _implPtr->target);

    // Shared subscriber: ViewSelectionChanged, filtered by trackedViewId
    _implPtr->selectionSub = _implPtr->views.onSelectionChanged(
      [this](ViewService::SelectionChanged const& ev)
      {
        if (ev.viewId != _implPtr->trackedViewId)
        {
          return;
        }

        _implPtr->cachedSnapshot = buildSnapshot(ev.selection);
        publishSnapshot();
      });

    _implPtr->tracksMutatedSub = _implPtr->mutation.onTracksMutated(
      [this](std::vector<TrackId> const& trackIds)
      {
        if (_implPtr->cachedSnapshot.trackIds.empty())
        {
          return;
        }

        bool intersect = false;

        for (auto const id : trackIds)
        {
          if (std::ranges::contains(_implPtr->cachedSnapshot.trackIds, id))
          {
            intersect = true;
            break;
          }
        }

        if (intersect)
        {
          _implPtr->cachedSnapshot = buildSnapshot(_implPtr->cachedSnapshot.trackIds);
          publishSnapshot();
        }
      });
  }

  TrackDetailProjection::~TrackDetailProjection() = default;

  TrackDetailSnapshot TrackDetailProjection::snapshot() const
  {
    return _implPtr->cachedSnapshot;
  }

  Subscription TrackDetailProjection::subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler)
  {
    handler(_implPtr->cachedSnapshot);

    auto const index = _implPtr->subscribers.size();
    _implPtr->subscribers.push_back(std::move(handler));

    return Subscription{[this, index] { _implPtr->subscribers[index] = {}; }};
  }

  void TrackDetailProjection::publishSnapshot()
  {
    _implPtr->cachedSnapshot.revision = ++_implPtr->revision;

    for (auto& sub : _implPtr->subscribers)
    {
      if (sub)
      {
        sub(_implPtr->cachedSnapshot);
      }
    }
  }

  TrackDetailSnapshot TrackDetailProjection::buildSnapshot(std::span<TrackId const> ids) const
  {
    auto snap = TrackDetailSnapshot{
      .selectionKind = selectionKindFromCount(ids.size()),
      .trackIds = {ids.begin(), ids.end()},
    };

    if (ids.empty())
    {
      return snap;
    }

    auto const txn = _implPtr->library.readTransaction();
    auto const trackReader = _implPtr->library.tracks().reader(txn);
    auto const manifestReader = _implPtr->library.manifest().reader(txn);
    auto const& dict = _implPtr->library.dictionary();

    auto fieldValues = std::array<std::vector<TrackFieldRawValue>, kTrackFieldCount>{};
    auto customMap = std::map<std::string, CustomAggregationState>{};
    std::size_t loadedCount = 0;

    for (auto const trackId : ids)
    {
      auto const optView = trackReader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      loadedCount++;
      aggregateFields(*optView, dict, &manifestReader, fieldValues);
      aggregateCustom(*optView, dict, customMap);

      if (ids.size() == 1)
      {
        if (auto const optPrimary = optView->coverArt().primary(); optPrimary)
        {
          snap.singleCoverArtId = optPrimary->resourceId;
        }

        for (auto const tagId : optView->tags())
        {
          snap.commonTagIds.push_back(tagId);
        }
      }
    }

    if (loadedCount == 0)
    {
      return snap;
    }

    auto fieldIter = snap.fields.begin();

    for (auto const& values : fieldValues)
    {
      *fieldIter = aggregate(values);
      ++fieldIter;
    }

    for (auto const& [key, state] : customMap)
    {
      auto item = CustomMetadataItem{
        .key = key,
        .value = {.optValue = state.mixed ? std::nullopt : state.optFirstValue, .mixed = state.mixed},
        .presentOnAll = (state.presentCount == loadedCount),
        .presentOnAny = true,
      };
      snap.customMetadata.push_back(std::move(item));
    }

    return snap;
  }
}
