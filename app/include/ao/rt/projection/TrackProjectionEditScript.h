// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackListProjection.h"
#include <ao/rt/TrackEditScript.h>

#include <vector>

namespace ao::rt
{
  TrackListProjectionDeltaBatch eraseTrackIds(delta::RegularTrackEditScript const& script);
  void appendProjectionInsertRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices);
  void appendProjectionRemoveRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices);
  void appendProjectionUpdateRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices);
} // namespace ao::rt
