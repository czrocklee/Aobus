// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt::test
{
  class MutableTrackSource final : public TrackSource
  {
  public:
    void addInitial(TrackId id) { _ids.push_back(id); }

    void setInitial(std::span<TrackId const> ids) { _ids.assign(ids.begin(), ids.end()); }

    void insert(TrackId id, std::size_t index)
    {
      REQUIRE(index <= _ids.size());
      _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
      notifyInserted(id, index);
    }

    void append(TrackId id) { insert(id, _ids.size()); }

    void update(TrackId id)
    {
      auto const optIndex = indexOf(id);
      REQUIRE(optIndex);
      notifyUpdated(id, *optIndex);
    }

    void remove(TrackId id)
    {
      auto const optIndex = indexOf(id);
      REQUIRE(optIndex);
      _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*optIndex));
      notifyRemoved(id, *optIndex);
    }

    void reset(std::span<TrackId const> ids = {})
    {
      _ids.assign(ids.begin(), ids.end());
      notifyReset();
    }

    void onReset() { notifyReset(); }

    void batchInsert(std::span<TrackId const> ids)
    {
      _ids.insert(_ids.end(), ids.begin(), ids.end());
      notifyInserted(ids);
    }

    void batchRemove(std::span<TrackId const> ids)
    {
      for (auto id : ids)
      {
        std::erase(_ids, id);
      }

      notifyRemoved(ids);
    }

    void batchUpdate(std::span<TrackId const> ids) { notifyUpdated(ids); }

    void singleInsert(TrackId id) { append(id); }

    void singleRemove(TrackId id) { remove(id); }

    void singleUpdate(TrackId id) { update(id); }

    std::size_t size() const override { return _ids.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override
    {
      if (auto it = std::ranges::find(_ids, id); it != _ids.end())
      {
        return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
      }

      return std::nullopt;
    }

  private:
    std::vector<TrackId> _ids;
  };

  struct TrackSourceObserverSpy final : public TrackSourceObserver
  {
    enum class EventKind : std::uint8_t
    {
      Reset,
      Inserted,
      Updated,
      Removed,
      BatchInserted,
      BatchUpdated,
      BatchRemoved,
    };

    struct Event final
    {
      EventKind kind;
      TrackId id{};
      std::size_t index = 0;
      std::vector<TrackId> batchIds{};
    };

    void onReset() override { events.push_back({.kind = EventKind::Reset}); }

    void onInserted(TrackId id, std::size_t index) override
    {
      events.push_back({.kind = EventKind::Inserted, .id = id, .index = index});
    }

    void onUpdated(TrackId id, std::size_t index) override
    {
      events.push_back({.kind = EventKind::Updated, .id = id, .index = index});
    }

    void onRemoved(TrackId id, std::size_t index) override
    {
      events.push_back({.kind = EventKind::Removed, .id = id, .index = index});
    }

    void onBulkInserted(std::span<TrackId const> ids) override
    {
      events.push_back({.kind = EventKind::BatchInserted, .batchIds = {ids.begin(), ids.end()}});
    }

    void onBulkUpdated(std::span<TrackId const> ids) override
    {
      events.push_back({.kind = EventKind::BatchUpdated, .batchIds = {ids.begin(), ids.end()}});
    }

    void onBulkRemoved(std::span<TrackId const> ids) override
    {
      events.push_back({.kind = EventKind::BatchRemoved, .batchIds = {ids.begin(), ids.end()}});
    }

    void clear() { events.clear(); }

    std::vector<Event> events;
  };
} // namespace ao::rt::test
