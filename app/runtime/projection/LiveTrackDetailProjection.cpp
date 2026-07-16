// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors
#include "runtime/TrackFieldReaderInternal.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/LiveTrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailProjection.h>

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
                         library::DictionaryStore const& dictionary,
                         library::FileManifestStore::Reader const* manifestReader,
                         std::array<std::vector<TrackFieldRawValue>, kTrackFieldCount>& fieldValues)
    {
      for (auto const& def : trackFieldDefinitions())
      {
        if (def.synthetic || def.category == TrackFieldCategory::Tag)
        {
          continue;
        }

        auto val = readTrackFieldRawValue(def.field, view, dictionary, manifestReader);
        trackFieldArrayAt(fieldValues, def.field).push_back(std::move(val));
      }
    }

    void aggregateCustom(library::TrackView const& view,
                         library::DictionaryStore const& dictionary,
                         std::map<std::string, CustomAggregationState>& customAggregates)
    {
      for (auto const& [dictionaryId, value] : view.customMetadata())
      {
        auto const key = std::string{dictionary.getOrDefault(dictionaryId)};

        if (key.empty())
        {
          continue;
        }

        auto& state = customAggregates[key];
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
  } // namespace

  struct LiveTrackDetailProjection::Impl final
  {
    DetailTarget target;
    ViewService& views;
    library::MusicLibrary const& library;
    WorkspaceService& workspace;
    LibraryChanges const& changes;

    TrackDetailSnapshot cachedSnapshot;
    std::uint64_t revision = 0;
    std::vector<std::move_only_function<void(TrackDetailSnapshot const&)>> subscribers;
    Subscription focusSub;
    Subscription selectionSub;
    Subscription tracksMutatedSub;
    ViewId trackedViewId = rt::kInvalidViewId;

    Impl(DetailTarget target,
         ViewService& views,
         library::MusicLibrary const& library,
         WorkspaceService& workspace,
         LibraryChanges const& changes)
      : target{std::move(target)}, views{views}, library{library}, workspace{workspace}, changes{changes}
    {
    }
  };

  LiveTrackDetailProjection::LiveTrackDetailProjection(DetailTarget target,
                                                       ViewService& views,
                                                       library::MusicLibrary const& library,
                                                       WorkspaceService& workspace,
                                                       LibraryChanges const& changes)
    : _implPtr{std::make_unique<Impl>(std::move(target), views, library, workspace, changes)}
  {
    std::visit(
      [this](auto const& target)
      {
        using T = std::decay_t<decltype(target)>;

        if constexpr (std::is_same_v<T, FocusedViewTarget>)
        {
          auto const layout = _implPtr->workspace.snapshot();
          _implPtr->trackedViewId = layout.activeViewId;

          if (_implPtr->trackedViewId != rt::kInvalidViewId)
          {
            auto const state = _implPtr->views.trackListState(_implPtr->trackedViewId);
            _implPtr->cachedSnapshot = buildSnapshot(state.selection);
          }

          _implPtr->focusSub = _implPtr->workspace.onChanged(
            [this](WorkspaceChanged const& changed)
            {
              auto const viewId = changed.snapshot.activeViewId;

              if (viewId == _implPtr->trackedViewId)
              {
                return;
              }

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

    _implPtr->tracksMutatedSub = _implPtr->changes.onChanged(
      [this](LibraryChangeSet const& changeSet)
      {
        if (_implPtr->cachedSnapshot.trackIds.empty())
        {
          return;
        }

        auto trackIds = changeSet.tracksInserted;
        trackIds.append_range(changeSet.tracksDeleted);
        trackIds.append_range(changeSet.tracksMutated);
        bool intersect = changeSet.libraryReset;

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

  LiveTrackDetailProjection::~LiveTrackDetailProjection() = default;

  TrackDetailSnapshot LiveTrackDetailProjection::snapshot() const
  {
    return _implPtr->cachedSnapshot;
  }

  Subscription LiveTrackDetailProjection::subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler)
  {
    handler(_implPtr->cachedSnapshot);

    auto const index = _implPtr->subscribers.size();
    _implPtr->subscribers.push_back(std::move(handler));

    return Subscription{[this, index] { _implPtr->subscribers[index] = {}; }};
  }

  void LiveTrackDetailProjection::publishSnapshot()
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

  TrackDetailSnapshot LiveTrackDetailProjection::buildSnapshot(std::span<TrackId const> ids) const
  {
    auto snap = TrackDetailSnapshot{
      .selectionKind = selectionKindFromCount(ids.size()),
      .trackIds = {ids.begin(), ids.end()},
    };

    if (ids.empty())
    {
      return snap;
    }

    auto const transaction = _implPtr->library.readTransaction();
    auto const trackReader = _implPtr->library.tracks().reader(transaction);
    auto const manifestReader = _implPtr->library.manifest().reader(transaction);
    auto const& dictionary = _implPtr->library.dictionary();

    auto fieldValues = std::array<std::vector<TrackFieldRawValue>, kTrackFieldCount>{};
    auto customAggregates = std::map<std::string, CustomAggregationState>{};
    std::size_t loadedCount = 0;

    for (auto const trackId : ids)
    {
      auto const optView = storageValueOrNullopt(
        trackReader.get(trackId, library::TrackStore::Reader::LoadMode::Both), "Failed to build track detail snapshot");

      if (!optView)
      {
        continue;
      }

      loadedCount++;
      aggregateFields(*optView, dictionary, &manifestReader, fieldValues);
      aggregateCustom(*optView, dictionary, customAggregates);

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

    for (auto const& [key, state] : customAggregates)
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
} // namespace ao::rt
