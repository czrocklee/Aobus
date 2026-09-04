// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackEditScript.h>
#include <ao/rt/projection/TrackListProjection.h>

namespace ao::rt
{
  TrackListProjectionDeltaBatch eraseTrackIds(delta::RegularTrackEditScript const& script);
} // namespace ao::rt
