// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackDetailProjection.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ViewService.h"

#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <algorithm>

namespace ao::app
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

    template<class T>
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
    EventBus& events;
    ao::library::MusicLibrary& library;
    TrackDetailSnapshot cachedSnapshot;
    std::uint64_t revision = 0;
    std::vector<std::move_only_function<void(TrackDetailSnapshot const&)>> subscribers;
    Subscription focusSub;
    Subscription selectionSub;
    Subscription tracksMutatedSub;
    ViewId trackedViewId{};
  };

  TrackDetailProjection::TrackDetailProjection(DetailTarget target,
                                               ViewService& views,
                                               EventBus& events,
                                               ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(std::move(target), views, events, library)}
  {
    std::visit(
      [this](auto const& target)
      {
        using T = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<T, FocusedViewTarget>)
        {
          _impl->focusSub = _impl->events.subscribe<FocusedViewChanged>(
            [this](FocusedViewChanged const& ev)
            {
              _impl->trackedViewId = ev.viewId;
              if (ev.viewId == ViewId{})
              {
                return;
              }
              auto const state = _impl->views.trackListState(ev.viewId);
              _impl->cachedSnapshot = buildSnapshot(state.selection);
              publishSnapshot();
            });
        }
        else if constexpr (std::is_same_v<T, ExplicitViewTarget>)
        {
          _impl->trackedViewId = target.viewId;
          auto const state = _impl->views.trackListState(target.viewId);
          _impl->cachedSnapshot = buildSnapshot(state.selection);
        }
        else if constexpr (std::is_same_v<T, ExplicitSelectionTarget>)
        {
          _impl->cachedSnapshot = buildSnapshot(target.trackIds);
        }
      },
      _impl->target);

    // Shared subscriber: ViewSelectionChanged, filtered by trackedViewId
    _impl->selectionSub = _impl->events.subscribe<ViewSelectionChanged>(
      [this](ViewSelectionChanged const& ev)
      {
        if (ev.viewId != _impl->trackedViewId)
        {
          return;
        }
        _impl->cachedSnapshot = buildSnapshot(ev.selection);
        publishSnapshot();
      });

    _impl->tracksMutatedSub = _impl->events.subscribe<TracksMutated>(
      [this](TracksMutated const& ev)
      {
        if (_impl->cachedSnapshot.trackIds.empty())
        {
          return;
        }

        bool intersect = false;
        for (auto const id : ev.trackIds)
        {
          if (std::ranges::find(_impl->cachedSnapshot.trackIds, id) != _impl->cachedSnapshot.trackIds.end())
          {
            intersect = true;
            break;
          }
        }

        if (intersect)
        {
          _impl->cachedSnapshot = buildSnapshot(_impl->cachedSnapshot.trackIds);
          publishSnapshot();
        }
      });
  }

  TrackDetailProjection::~TrackDetailProjection() = default;

  TrackDetailSnapshot TrackDetailProjection::snapshot() const
  {
    return _impl->cachedSnapshot;
  }

  Subscription TrackDetailProjection::subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler)
  {
    handler(_impl->cachedSnapshot);

    auto const index = _impl->subscribers.size();
    _impl->subscribers.push_back(std::move(handler));

    return Subscription{[this, index] { _impl->subscribers[index] = {}; }};
  }

  void TrackDetailProjection::publishSnapshot()
  {
    _impl->cachedSnapshot.revision = ++_impl->revision;
    for (auto& sub : _impl->subscribers)
    {
      if (sub)
      {
        sub(_impl->cachedSnapshot);
      }
    }
  }

  TrackDetailSnapshot TrackDetailProjection::buildSnapshot(std::span<ao::TrackId const> ids) const
  {
    auto snap = TrackDetailSnapshot{
      .selectionKind = selectionKindFromCount(ids.size()),
      .trackIds = {ids.begin(), ids.end()},
    };

    if (ids.empty())
    {
      return snap;
    }

    auto const txn = _impl->library.readTransaction();
    auto const reader = _impl->library.tracks().reader(txn);
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
      auto const optView = reader.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);
      if (!optView)
      {
        continue;
      }

      codecIds.push_back(optView->property().codecId());
      sampleRates.push_back(optView->property().sampleRate());
      channelss.push_back(optView->property().channels());
      bitDepths.push_back(optView->property().bitDepth());
      durations.push_back(optView->property().durationMs());

      titles.push_back(std::string{optView->metadata().title()});

      auto const artistId = optView->metadata().artistId();
      artists.push_back(artistId != ao::DictionaryId{0} ? std::string{_impl->library.dictionary().get(artistId)}
                                                        : std::string{});

      auto const albumId = optView->metadata().albumId();
      albums.push_back(albumId != ao::DictionaryId{0} ? std::string{_impl->library.dictionary().get(albumId)}
                                                      : std::string{});

      if (ids.size() == 1)
      {
        snap.singleCoverArtId = ao::ResourceId{optView->metadata().coverArtId()};
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
