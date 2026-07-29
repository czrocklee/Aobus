// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/TrackSourceTestSupport.h"

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
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
  MutableTrackSource::MutableTrackSource() = default;
  MutableTrackSource::~MutableTrackSource() = default;

  void MutableTrackSource::addInitial(TrackId id)
  {
    _ids.push_back(id);
  }

  void MutableTrackSource::setInitial(std::span<TrackId const> ids)
  {
    _ids.assign(ids.begin(), ids.end());
  }

  void MutableTrackSource::insert(TrackId id, std::size_t index)
  {
    REQUIRE(index <= _ids.size());
    _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
    notifyInserted(id, index);
  }

  void MutableTrackSource::append(TrackId id)
  {
    insert(id, _ids.size());
  }

  void MutableTrackSource::update(TrackId id)
  {
    auto const optIndex = indexOf(id);
    REQUIRE(optIndex);
    notifyUpdated(id, *optIndex);
  }

  void MutableTrackSource::remove(TrackId id)
  {
    auto const optIndex = indexOf(id);
    REQUIRE(optIndex);
    _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*optIndex));
    notifyRemoved(id, *optIndex);
  }

  void MutableTrackSource::reset(std::span<TrackId const> ids)
  {
    _ids.assign(ids.begin(), ids.end());
    notifyReset();
  }

  void MutableTrackSource::emitReset()
  {
    notifyReset();
  }

  void MutableTrackSource::batchInsert(std::span<TrackId const> ids)
  {
    _ids.append_range(ids);
    notifyInserted(ids);
  }

  void MutableTrackSource::batchRemove(std::span<TrackId const> ids)
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

      std::ignore = publishDelta(std::move(*optBatch), previousSize);
    }
  }

  void MutableTrackSource::batchUpdate(std::span<TrackId const> ids)
  {
    notifyUpdated(ids);
  }

  void MutableTrackSource::updateByIdentity(TrackId id)
  {
    notifyUpdated(id);
  }

  void MutableTrackSource::singleInsert(TrackId id)
  {
    append(id);
  }

  void MutableTrackSource::singleRemove(TrackId id)
  {
    remove(id);
  }

  void MutableTrackSource::singleUpdate(TrackId id)
  {
    update(id);
  }

  void MutableTrackSource::replaceWithBatch(std::span<TrackId const> ids, TrackSourceDelta batch)
  {
    auto const previousSize = _ids.size();
    _ids.assign(ids.begin(), ids.end());
    std::ignore = publishDelta(std::move(batch), previousSize);
  }

  void MutableTrackSource::publishBatch(TrackSourceDelta batch)
  {
    std::ignore = publishDelta(std::move(batch), _ids.size());
  }

  std::size_t MutableTrackSource::size() const
  {
    return _ids.size();
  }

  TrackId MutableTrackSource::trackIdAt(std::size_t index) const
  {
    return _ids.at(index);
  }

  std::optional<std::size_t> MutableTrackSource::indexOf(TrackId id) const
  {
    if (auto it = std::ranges::find(_ids, id); it != _ids.end())
    {
      return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
    }

    return std::nullopt;
  }

  std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::span<TrackId const> ids)
  {
    auto sourcePtr = std::make_shared<MutableTrackSource>();
    sourcePtr->setInitial(ids);
    return sourcePtr;
  }

  std::shared_ptr<MutableTrackSource> makeMutableTrackSource(std::initializer_list<TrackId> ids)
  {
    return makeMutableTrackSource(std::span{ids.begin(), ids.size()});
  }

  std::vector<TrackId> sourceTrackIds(TrackSource const& source)
  {
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(source.size());

    for (std::size_t index = 0; index < source.size(); ++index)
    {
      trackIds.push_back(source.trackIdAt(index));
    }

    return trackIds;
  }

  delta::RegularTrackEditScript const& sourceEditScript(TrackSourceDelta const& message)
  {
    return std::get<delta::RegularTrackEditScript>(message);
  }

  TrackSourceBatchSpy::TrackSourceBatchSpy(TrackSource& source)
    : _subscription{source.subscribe([this](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); })}
  {
  }

  TrackSourceBatchSpy::~TrackSourceBatchSpy() = default;

  void TrackSourceBatchSpy::clear()
  {
    batches.clear();
  }
} // namespace ao::rt::test
