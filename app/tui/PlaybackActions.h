// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListEntry.h"
#include <ao/CoreIds.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::tui
{
  bool playSelected(rt::PlaybackService& playback,
                    std::vector<TrackListEntry> const& tracks,
                    std::int32_t selected,
                    ListId sourceListId);

  bool togglePlayback(rt::PlaybackService& playback,
                      std::vector<TrackListEntry> const& tracks,
                      std::int32_t selected,
                      ListId sourceListId);

  void seekBy(rt::PlaybackService& playback, std::chrono::milliseconds delta);
  void changeVolume(rt::PlaybackService& playback, float delta);
} // namespace ao::tui
