// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace ao::library
{
  inline Result<std::pair<TrackId, TrackView>> createPreparedTrackRecord(TrackStore::Writer& writer,
                                                                         TrackBuilder::PreparedHot const& preparedHot,
                                                                         TrackBuilder::PreparedCold const& preparedCold)
  {
    return writer.createHotCold(preparedHot.size(),
                                preparedCold.size(),
                                [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                {
                                  preparedHot.writeTo(hot);
                                  preparedCold.writeTo(cold);
                                });
  }

  inline Result<> updatePreparedHotTrackRecord(TrackStore::Writer& writer,
                                               TrackId trackId,
                                               TrackBuilder::PreparedHot const& preparedHot)
  {
    return writer.updateHot(trackId, preparedHot.size(), [&](std::span<std::byte> hot) { preparedHot.writeTo(hot); });
  }

  inline Result<> updatePreparedColdTrackRecord(TrackStore::Writer& writer,
                                                TrackId trackId,
                                                TrackBuilder::PreparedCold const& preparedCold)
  {
    return writer.updateCold(
      trackId, preparedCold.size(), [&](std::span<std::byte> cold) { preparedCold.writeTo(cold); });
  }

  inline Result<> updatePreparedTrackRecord(TrackStore::Writer& writer,
                                            TrackId trackId,
                                            TrackBuilder::PreparedHot const& preparedHot,
                                            TrackBuilder::PreparedCold const& preparedCold)
  {
    auto hotResult = updatePreparedHotTrackRecord(writer, trackId, preparedHot);

    if (!hotResult)
    {
      return std::unexpected{hotResult.error()};
    }

    auto coldResult = updatePreparedColdTrackRecord(writer, trackId, preparedCold);

    if (!coldResult)
    {
      return std::unexpected{coldResult.error()};
    }

    return {};
  }
} // namespace ao::library
