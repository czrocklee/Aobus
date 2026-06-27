// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
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
      REQUIRE(optIndex.has_value());
      notifyUpdated(id, *optIndex);
    }

    void remove(TrackId id)
    {
      auto const optIndex = indexOf(id);
      REQUIRE(optIndex.has_value());
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
} // namespace ao::rt::test
