// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListEntry.h"
#include <ao/rt/ViewIds.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace ao::rt
{
  class PlaybackSequenceService;
  class PlaybackService;
} // namespace ao::rt

namespace ao::tui
{
  bool playSelected(rt::PlaybackSequenceService& sequence,
                    std::vector<TrackListEntry> const& tracks,
                    std::int32_t selected,
                    rt::ViewId sourceViewId);

  bool togglePlayback(rt::PlaybackService& playback,
                      rt::PlaybackSequenceService& sequence,
                      std::vector<TrackListEntry> const& tracks,
                      std::int32_t selected,
                      rt::ViewId sourceViewId);

  void seekBy(rt::PlaybackService& playback, std::chrono::milliseconds delta);
  void changeVolume(rt::PlaybackService& playback, float delta);
} // namespace ao::tui
