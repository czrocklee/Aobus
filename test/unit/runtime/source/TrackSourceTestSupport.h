// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
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

    void emitReset() { notifyReset(); }

    void batchInsert(std::span<TrackId const> ids)
    {
      _ids.append_range(ids);
      notifyInserted(ids);
    }

    void batchRemove(std::span<TrackId const> ids)
    {
      auto const previousSize = _ids.size();
      auto builder = TrackSourceDeltaBuilder{previousSize};

      for (auto id : ids)
      {
        if (auto const optIndex = indexOf(id); optIndex)
        {
          builder.remove(*optIndex, id);
        }
      }

      if (auto optBatch = builder.build(); optBatch)
      {
        for (auto const id : ids)
        {
          std::erase(_ids, id);
        }

        std::ignore = publishDeltaBatch(std::move(*optBatch), previousSize);
      }
    }

    void batchUpdate(std::span<TrackId const> ids) { notifyUpdated(ids); }

    void singleInsert(TrackId id) { append(id); }

    void singleRemove(TrackId id) { remove(id); }

    void singleUpdate(TrackId id) { update(id); }

    void replaceWithBatch(std::span<TrackId const> ids, TrackSourceDeltaBatch batch)
    {
      auto const previousSize = _ids.size();
      _ids.assign(ids.begin(), ids.end());
      std::ignore = publishDeltaBatch(std::move(batch), previousSize);
    }

    void publishBatch(TrackSourceDeltaBatch batch) { std::ignore = publishDeltaBatch(std::move(batch), _ids.size()); }

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

  inline std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::span<TrackId const> const ids)
  {
    auto sourcePtr = std::make_shared<MutableTrackSource>();
    sourcePtr->setInitial(ids);
    return sourcePtr;
  }

  inline std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::initializer_list<TrackId> const ids)
  {
    return makeMutableTrackSource(std::span{ids.begin(), ids.size()});
  }

  inline std::vector<TrackId> sourceTrackIds(TrackSource const& source)
  {
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(source.size());

    for (std::size_t index = 0; index < source.size(); ++index)
    {
      trackIds.push_back(source.trackIdAt(index));
    }

    return trackIds;
  }

  // Recorded batches are intentionally public as the spy's assertion surface.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  class TrackSourceBatchSpy final
  {
  public:
    explicit TrackSourceBatchSpy(TrackSource& source)
      : _subscription{source.subscribe([this](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); })}
    {
    }

    ~TrackSourceBatchSpy() = default;

    TrackSourceBatchSpy(TrackSourceBatchSpy const&) = delete;
    TrackSourceBatchSpy& operator=(TrackSourceBatchSpy const&) = delete;
    TrackSourceBatchSpy(TrackSourceBatchSpy&&) = delete;
    TrackSourceBatchSpy& operator=(TrackSourceBatchSpy&&) = delete;

    void clear() { batches.clear(); }

    // NOLINTNEXTLINE(aobus-readability-identifier-naming-extensions)
    std::vector<TrackSourceDeltaBatch> batches;

  private:
    async::Subscription _subscription;
  };
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
} // namespace ao::rt::test
