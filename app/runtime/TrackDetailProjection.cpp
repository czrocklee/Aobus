// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

      auto result = AggregateValue<T>{.optValue = values.front()};

      for (std::size_t i = 1; i < values.size(); ++i)
      {
        if (values[i] != *result.optValue)
        {
          result.mixed = true;
          break;
        }
      }

      return result;
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
          _implPtr->focusSub = _implPtr->workspace.onFocusedViewChanged(
            [this](ViewId viewId)
            {
              _implPtr->trackedViewId = viewId;

              if (viewId == rt::kInvalidViewId)
              {
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
    auto const reader = _implPtr->library.tracks().reader(txn);
    auto codecIds = std::vector<std::uint16_t>{};
    auto sampleRates = std::vector<std::uint32_t>{};
    auto channelss = std::vector<std::uint8_t>{};
    auto bitDepths = std::vector<std::uint8_t>{};
    auto durations = std::vector<std::uint32_t>{};

    auto titles = std::vector<std::string>{};
    auto artists = std::vector<std::string>{};
    auto albums = std::vector<std::string>{};

    for (auto const trackId : ids)
    {
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      codecIds.push_back(optView->property().codecId());
      sampleRates.push_back(optView->property().sampleRate());
      channelss.push_back(optView->property().channels());
      bitDepths.push_back(optView->property().bitDepth());
      durations.push_back(optView->property().durationMs());

      titles.emplace_back(optView->metadata().title());

      auto const artistId = optView->metadata().artistId();
      artists.push_back(artistId != kInvalidDictionaryId ? std::string{_implPtr->library.dictionary().get(artistId)}
                                                         : std::string{});

      auto const albumId = optView->metadata().albumId();
      albums.push_back(albumId != kInvalidDictionaryId ? std::string{_implPtr->library.dictionary().get(albumId)}
                                                       : std::string{});

      if (ids.size() == 1)
      {
        snap.singleCoverArtId = ResourceId{optView->metadata().coverArtId()};

        for (auto const tagId : optView->tags())
        {
          snap.commonTagIds.push_back(tagId);
        }
      }
    }

    snap.audio.codecId = aggregate(codecIds);
    snap.audio.sampleRate = aggregate(sampleRates);
    snap.audio.channels = aggregate(channelss);
    snap.audio.bitDepth = aggregate(bitDepths);
    snap.audio.durationMs = aggregate(durations);

    snap.title = aggregate(titles);
    snap.artist = aggregate(artists);
    snap.album = aggregate(albums);

    return snap;
  }
}
