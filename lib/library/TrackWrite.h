// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>

namespace ao::library
{
  Result<TrackId> createPreparedTrackRecord(TrackStore::Writer& writer,
                                            TrackBuilder::PreparedHot const& preparedHot,
                                            TrackBuilder::PreparedCold const& preparedCold);

  Result<> updatePreparedHotTrackRecord(TrackStore::Writer& writer,
                                        TrackId trackId,
                                        TrackBuilder::PreparedHot const& preparedHot);

  Result<> updatePreparedColdTrackRecord(TrackStore::Writer& writer,
                                         TrackId trackId,
                                         TrackBuilder::PreparedCold const& preparedCold);

  Result<> updatePreparedTrackRecord(TrackStore::Writer& writer,
                                     TrackId trackId,
                                     TrackBuilder::PreparedHot const& preparedHot,
                                     TrackBuilder::PreparedCold const& preparedCold);
} // namespace ao::library
