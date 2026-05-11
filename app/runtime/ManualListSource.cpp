// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ManualListSource.h"

#include <ao/library/ListView.h>

#include <algorithm>

namespace ao::rt
{
  ManualListSource::ManualListSource(ao::library::ListView const& view, TrackSource* source)
    : _source{source}
  {
    _trackIds.reserve(view.tracks().size());

    for (auto const& id : view.tracks())
    {
      _trackIds.push_back(id);
    }

    if (_source != nullptr)
    {
      _source->attach(this);
    }
  }

  ManualListSource::ManualListSource() = default;

  ManualListSource::~ManualListSource()
  {
    if (_source != nullptr)
    {
      _source->detach(this);
    }
  }

  void ManualListSource::reloadFromListView(ao::library::ListView const& view)
  {
    _trackIds.clear();
    _trackIds.reserve(view.tracks().size());

    for (auto const& id : view.tracks())
    {
      if (_source == nullptr || _source->indexOf(id))
      {
        _trackIds.push_back(id);
      }
    }

    TrackSource::notifyReset();
  }

  void ManualListSource::onReset()
  {
    if (_source == nullptr)
    {
      return;
    }

    // Filter existing members against new source state
    auto next = std::vector<TrackId>{};

    for (auto const& id : _trackIds)
    {
      if (_source->indexOf(id))
      {
        next.push_back(id);
      }
    }

    _trackIds = std::move(next);
    TrackSource::notifyReset();
  }

  void ManualListSource::onInserted(TrackId /*id*/, std::size_t /*index*/)
  {
  }

  void ManualListSource::onUpdated(TrackId id, std::size_t /*index*/)
  {
    if (auto const myIndex = indexOf(id))
    {
      TrackSource::notifyUpdated(id, *myIndex);
    }
  }

  void ManualListSource::onRemoved(TrackId id, std::size_t /*index*/)
  {
    if (auto it = std::ranges::find(_trackIds, id); it != _trackIds.end())
    {
      auto const myIndex = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      _trackIds.erase(it);
      TrackSource::notifyRemoved(id, myIndex);
    }
  }

  void ManualListSource::onInserted(std::span<TrackId const> /*ids*/)
  {
  }

  void ManualListSource::onUpdated(std::span<TrackId const> ids)
  {
    auto matched = std::vector<TrackId>{};

    for (auto const id : ids)
    {
      if (contains(id))
      {
        matched.push_back(id);
      }
    }

    if (!matched.empty())
    {
      TrackSource::notifyUpdated(matched);
    }
  }

  void ManualListSource::onRemoved(std::span<TrackId const> ids)
  {
    std::vector<TrackId> removed;

    for (auto id : ids)
    {
      if (auto it = std::ranges::find(_trackIds, id); it != _trackIds.end())
      {
        _trackIds.erase(it);
        removed.push_back(id);
      }
    }

    if (!removed.empty())
    {
      TrackSource::notifyRemoved(removed);
    }
  }

  bool ManualListSource::contains(TrackId id) const
  {
    return std::ranges::contains(_trackIds, id);
  }

  std::optional<std::size_t> ManualListSource::indexOf(TrackId id) const
  {
    auto const it = std::ranges::find(_trackIds, id);

    if (it == _trackIds.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
  }
}
