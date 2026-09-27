// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/TrackSourceTestSupport.h"

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <concepts>
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
    publishInserted(index, id);
  }

  void MutableTrackSource::append(TrackId id)
  {
    insert(id, _ids.size());
  }

  void MutableTrackSource::update(TrackId id)
  {
    auto const optIndex = indexOf(id);
    REQUIRE(optIndex);
    publishUpdated(*optIndex, id);
  }

  void MutableTrackSource::remove(TrackId id)
  {
    auto const optIndex = indexOf(id);
    REQUIRE(optIndex);
    _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*optIndex));
    publishRemoved(*optIndex, id);
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
    publishMatchedBatch<delta::InsertRange>(ids);
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

      std::ignore = tryPublishDelta(std::move(*optBatch), previousSize);
    }
  }

  void MutableTrackSource::batchUpdate(std::span<TrackId const> ids)
  {
    publishMatchedBatch<delta::UpdateRange>(ids);
  }

  void MutableTrackSource::updateByIdentity(TrackId id)
  {
    if (auto const optIndex = indexOf(id); optIndex)
    {
      publishUpdated(*optIndex, id);
    }
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
    std::ignore = tryPublishDelta(std::move(batch), previousSize);
  }

  void MutableTrackSource::publishBatch(TrackSourceDelta batch)
  {
    std::ignore = tryPublishDelta(std::move(batch), _ids.size());
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

  void MutableTrackSource::publishInserted(std::size_t const index, TrackId const id)
  {
    // The caller inserted into _ids first, so the stored size is at least one.
    std::ignore = tryPublishDelta(
      delta::RegularTrackEditScript{.edits = {delta::InsertRange{.start = index, .trackIds = {id}}}}, _ids.size() - 1);
  }

  void MutableTrackSource::publishUpdated(std::size_t const index, TrackId const id)
  {
    std::ignore = tryPublishDelta(
      delta::RegularTrackEditScript{.edits = {delta::UpdateRange{.start = index, .trackIds = {id}}}}, _ids.size());
  }

  void MutableTrackSource::publishRemoved(std::size_t const index, TrackId const id)
  {
    // The caller erased the id first, and a vector cannot reach size_t's
    // maximum, so the increment cannot overflow.
    std::ignore = tryPublishDelta(
      delta::RegularTrackEditScript{.edits = {delta::RemoveRange{.start = index, .trackIds = {id}}}}, _ids.size() + 1);
  }

  template<typename Range>
    requires std::same_as<Range, delta::InsertRange> || std::same_as<Range, delta::UpdateRange>
  void MutableTrackSource::publishMatchedBatch(std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto script = delta::RegularTrackEditScript{};
    std::size_t matchedCount = 0;

    for (std::size_t index = 0; index < _ids.size(); ++index)
    {
      auto const trackId = _ids[index];

      if (!std::ranges::contains(ids, trackId))
      {
        continue;
      }

      ++matchedCount;

      if (!script.edits.empty())
      {
        if (auto& range = std::get<Range>(script.edits.back()); range.start + range.trackIds.size() == index)
        {
          range.trackIds.push_back(trackId);
          continue;
        }
      }

      script.edits.emplace_back(Range{.start = index, .trackIds = {trackId}});
    }

    if (matchedCount == 0)
    {
      return;
    }

    // Every match occupies one distinct index, so the insert-size subtraction
    // cannot underflow.
    auto const previousSize = std::same_as<Range, delta::InsertRange> ? _ids.size() - matchedCount : _ids.size();
    std::ignore = tryPublishDelta(std::move(script), previousSize);
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

  TrackSourceBatchSpy::TrackSourceBatchSpy(TrackSource const& source)
    : _subscription{source.subscribe([this](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); })}
  {
  }

  TrackSourceBatchSpy::~TrackSourceBatchSpy() = default;

  void TrackSourceBatchSpy::clear()
  {
    batches.clear();
  }
} // namespace ao::rt::test
