// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackSourceDelta.h"
#include <ao/Error.h>
#include <ao/rt/TrackEditScript.h>

#include <cstdint>

namespace ao::rt
{
  enum class TrackSourceBatchKind : std::uint8_t
  {
    Regular,
    Reset,
    Invalidated,
    Invalid,
  };

  TrackSourceBatchKind classifyTrackSourceBatch(TrackSourceDeltaBatch const& batch) noexcept;
  Result<delta::RegularTrackEditScript> regularTrackEditScriptOf(TrackSourceDeltaBatch const& batch);
  TrackSourceDeltaBatch sourceBatchOf(delta::RegularTrackEditScript const& script);
} // namespace ao::rt
