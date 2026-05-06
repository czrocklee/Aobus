// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackDetailProjection.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ViewRegistry.h"

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
    ViewRegistry& views;
    EventBus& events;
    ao::library::MusicLibrary& library;
    TrackDetailSnapshot cachedSnapshot;
    std::uint64_t revision = 0;
    std::vector<std::move_only_function<void(TrackDetailSnapshot const&)>> subscribers;
    Subscription focusSub;
    Subscription selectionSub;
  };

  TrackDetailProjection::TrackDetailProjection(DetailTarget target,
                                               ViewRegistry& views,
                                               EventBus& events,
                                               ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(std::move(target), views, events, library)}
  {
    std::visit(
      [this](auto const& t)
      {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, FocusedViewTarget>)
        {
          _impl->focusSub = _impl->events.subscribe<FocusedViewChanged>(
            [this](FocusedViewChanged const& ev)
            {
              _impl->selectionSub.reset();
              if (ev.viewId == ViewId{})
              {
                return;
              }
              auto& state = _impl->views.trackListState(ev.viewId);
              _impl->selectionSub = state.subscribe(
                [this](TrackListViewState const& s)
                {
                  _impl->cachedSnapshot = buildSnapshot(s.selection);
                  publishSnapshot();
                });
            });
        }
        else if constexpr (std::is_same_v<T, ExplicitViewTarget>)
        {
          auto& state = _impl->views.trackListState(t.viewId);
          _impl->selectionSub = state.subscribe(
            [this](TrackListViewState const& s)
            {
              _impl->cachedSnapshot = buildSnapshot(s.selection);
              publishSnapshot();
            });
        }
        else if constexpr (std::is_same_v<T, ExplicitSelectionTarget>)
        {
          _impl->cachedSnapshot = buildSnapshot(t.trackIds);
        }
      },
      _impl->target);
  }

  TrackDetailProjection::~TrackDetailProjection() = default;

  TrackDetailSnapshot TrackDetailProjection::snapshot() const
  {
    return _impl->cachedSnapshot;
  }

  Subscription TrackDetailProjection::subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler,
                                                StoreDeliveryMode mode)
  {
    if (mode == StoreDeliveryMode::ReplayCurrent)
    {
      handler(_impl->cachedSnapshot);
    }

    auto index = _impl->subscribers.size();
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

    auto txn = _impl->library.readTransaction();
    auto reader = _impl->library.tracks().reader(txn);

    auto codecIds = std::vector<std::uint16_t>{};
    auto sampleRates = std::vector<std::uint32_t>{};
    auto channelss = std::vector<std::uint8_t>{};
    auto bitDepths = std::vector<std::uint8_t>{};
    auto durations = std::vector<std::uint32_t>{};

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

      if (ids.size() == 1)
      {
        snap.singleCoverArtId = ao::ResourceId{static_cast<std::uint32_t>(optView->metadata().coverArtId())};
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

    return snap;
  }
}
