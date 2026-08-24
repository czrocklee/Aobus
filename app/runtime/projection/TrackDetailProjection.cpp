// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors
#include <ao/rt/projection/TrackDetailProjection.h>

#include "runtime/TrackFieldReaderInternal.h"
#include <ao/CoreIds.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
    void addAggregateValue(AggregateValue<T>& aggregate, T value)
    {
      if (aggregate.mixed)
      {
        return;
      }

      if (!aggregate.optValue)
      {
        aggregate.optValue = std::move(value);
        return;
      }

      if (*aggregate.optValue != value)
      {
        aggregate.optValue.reset();
        aggregate.mixed = true;
      }
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
                         std::array<AggregateValue<TrackFieldRawValue>, kTrackFieldCount>& fieldAggregates)
    {
      for (auto const& def : trackFieldDefinitions())
      {
        if (def.synthetic || def.category == TrackFieldCategory::Tag)
        {
          continue;
        }

        auto& aggregate = trackFieldArrayAt(fieldAggregates, def.field);

        if (aggregate.mixed)
        {
          continue;
        }

        addAggregateValue(aggregate, readTrackFieldRawValue(def.field, view, dictionary, manifestReader));
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

    void populateSingleSnapshot(TrackDetailSnapshot& snapshot,
                                library::TrackView const& view,
                                library::DictionaryStore const& dictionary,
                                library::FileManifestStore::Reader const* manifestReader)
    {
      for (auto const& definition : trackFieldDefinitions())
      {
        if (definition.synthetic || definition.category == TrackFieldCategory::Tag)
        {
          continue;
        }

        trackFieldArrayAt(snapshot.fields, definition.field).optValue =
          readTrackFieldRawValue(definition.field, view, dictionary, manifestReader);
      }

      for (auto const& [dictionaryId, value] : view.customMetadata())
      {
        auto const key = std::string{dictionary.getOrDefault(dictionaryId)};

        if (!key.empty())
        {
          snapshot.customMetadata.push_back(CustomMetadataItem{
            .key = key,
            .value = {.optValue = std::string{value}},
            .presentOnAll = true,
            .presentOnAny = true,
          });
        }
      }

      std::ranges::sort(snapshot.customMetadata, {}, &CustomMetadataItem::key);

      if (auto const optPrimary = view.coverArt().primary(); optPrimary)
      {
        snapshot.singleCoverArtId = optPrimary->resourceId;
      }

      snapshot.commonTagIds.assign(view.tags().begin(), view.tags().end());
    }
  } // namespace

  struct TrackDetailProjection::Impl final
  {
    DetailTarget target;
    ViewService& views;
    library::MusicLibrary const& library;
    WorkspaceService& workspace;
    LibraryChanges const& changes;

    TrackDetailSnapshot cachedSnapshot;
    async::Signal<TrackDetailSnapshot const&> changedSignal;
    async::Subscription focusSub;
    async::Subscription selectionSub;
    async::Subscription viewDestroyedSub;
    async::Subscription tracksMutatedSub;
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

  TrackDetailProjection::TrackDetailProjection(DetailTarget target,
                                               ViewService& views,
                                               library::MusicLibrary const& library,
                                               WorkspaceService& workspace,
                                               LibraryChanges const& changes)
    : _implPtr{std::make_unique<Impl>(std::move(target), views, library, workspace, changes)}
  {
    std::visit([this](auto const& selectedTarget) { initializeTarget(selectedTarget); }, _implPtr->target);

    _implPtr->viewDestroyedSub = _implPtr->views.onViewDestroyed([this](ViewService::ViewDestroyed const& event)
                                                                 { handleViewDestroyed(event.viewId); });

    // Shared subscriber: ViewSelectionChanged, filtered by trackedViewId
    _implPtr->selectionSub = _implPtr->views.onSelectionChanged(
      [this](ViewService::SelectionChanged const& event) { handleSelectionChanged(event.viewId, event.selection); });

    _implPtr->tracksMutatedSub =
      _implPtr->changes.onChanged([this](LibraryChangeSet const& changeSet) { handleLibraryChanged(changeSet); });
  }

  TrackDetailProjection::~TrackDetailProjection() = default;

  TrackDetailSnapshot TrackDetailProjection::snapshot() const
  {
    return _implPtr->cachedSnapshot;
  }

  async::Subscription TrackDetailProjection::subscribe(
    compat::MoveOnlyFunction<void(TrackDetailSnapshot const&)> handler)
  {
    handler(_implPtr->cachedSnapshot);
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  void TrackDetailProjection::initializeTarget(FocusedViewTarget const& /*target*/)
  {
    auto const layout = _implPtr->workspace.snapshot();
    _implPtr->trackedViewId = layout.activeViewId;

    if (_implPtr->trackedViewId != rt::kInvalidViewId)
    {
      auto const state = _implPtr->views.trackListState(_implPtr->trackedViewId);
      _implPtr->cachedSnapshot = buildSnapshot(state.selection);
    }

    _implPtr->focusSub = _implPtr->workspace.onChanged([this](WorkspaceChanged const& changed)
                                                       { handleFocusedViewChanged(changed.snapshot.activeViewId); });
  }

  void TrackDetailProjection::initializeTarget(ExplicitViewTarget const& target)
  {
    _implPtr->trackedViewId = target.viewId;
    auto const foundRes = _implPtr->views.findTrackListState(target.viewId);

    if (foundRes)
    {
      _implPtr->cachedSnapshot = buildSnapshot(foundRes->selection);
    }
  }

  void TrackDetailProjection::initializeTarget(ExplicitSelectionTarget const& target)
  {
    _implPtr->cachedSnapshot = buildSnapshot(target.trackIds);
  }

  void TrackDetailProjection::handleFocusedViewChanged(ViewId const viewId)
  {
    if (viewId == _implPtr->trackedViewId)
    {
      return;
    }

    _implPtr->trackedViewId = viewId;

    if (viewId == rt::kInvalidViewId)
    {
      refreshSnapshot({});
      return;
    }

    // The workspace snapshot can name a view that ViewService has already
    // dropped before this observer runs.
    auto const foundRes = _implPtr->views.findTrackListState(viewId);
    refreshSnapshot(foundRes ? std::span<TrackId const>{foundRes->selection} : std::span<TrackId const>{});
  }

  void TrackDetailProjection::handleViewDestroyed(ViewId const viewId)
  {
    if (viewId != _implPtr->trackedViewId)
    {
      return;
    }

    _implPtr->trackedViewId = rt::kInvalidViewId;
    refreshSnapshot({});
  }

  void TrackDetailProjection::handleSelectionChanged(ViewId const viewId, std::span<TrackId const> const ids)
  {
    if (viewId != _implPtr->trackedViewId)
    {
      return;
    }

    refreshSnapshot(ids);
  }

  void TrackDetailProjection::handleLibraryChanged(LibraryChangeSet const& changeSet)
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
      refreshSnapshot(_implPtr->cachedSnapshot.trackIds);
    }
  }

  void TrackDetailProjection::publishSnapshot()
  {
    _implPtr->changedSignal.emit(_implPtr->cachedSnapshot);
  }

  void TrackDetailProjection::refreshSnapshot(std::span<TrackId const> const ids)
  {
    _implPtr->cachedSnapshot = buildSnapshot(ids);
    publishSnapshot();
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

    auto const transaction = _implPtr->library.readTransaction();
    auto const trackReader = _implPtr->library.tracks().reader(transaction);
    auto const manifestReader = _implPtr->library.manifest().reader(transaction);
    auto const& dictionary = _implPtr->library.dictionary();

    if (ids.size() == 1)
    {
      auto const optView = trackReader.get(ids.front(), library::TrackStore::Reader::LoadMode::Both);

      if (optView && optView->isHotValid() && optView->isColdValid())
      {
        populateSingleSnapshot(snap, *optView, dictionary, &manifestReader);
      }

      return snap;
    }

    auto customAggregates = std::map<std::string, CustomAggregationState>{};
    std::size_t loadedCount = 0;

    for (auto const trackId : ids)
    {
      auto const optView = trackReader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView || !optView->isHotValid() || !optView->isColdValid())
      {
        continue;
      }

      loadedCount++;
      aggregateFields(*optView, dictionary, &manifestReader, snap.fields);
      aggregateCustom(*optView, dictionary, customAggregates);
    }

    if (loadedCount == 0)
    {
      return snap;
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
