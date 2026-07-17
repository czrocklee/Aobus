// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListEntry.h"
#include <ao/rt/ViewIds.h>

#include <cstdint>
#include <vector>

namespace ao::rt
{
  class PlaybackSequenceService;
} // namespace ao::rt

namespace ao::tui
{
  bool playSelected(rt::PlaybackSequenceService& sequence,
                    std::vector<TrackListEntry> const& tracks,
                    std::int32_t selected,
                    rt::ViewId sourceViewId);
} // namespace ao::tui
