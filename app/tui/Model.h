// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Transport.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackRow.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::tui
{
  struct LibraryNavItem final
  {
    ListId id{};
    std::string label{};
    std::string detail{};
    std::string completionText{};
  };

  struct TrackListItem final
  {
    TrackId id{};
    ResourceId coverArtId{kInvalidResourceId};
    rt::TrackRow row{};
    std::string label{};
    std::string detail{};
  };

  struct TrackDetailLine final
  {
    std::string label{};
    std::string value{};
  };

  struct QualityIndicatorStyle final
  {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::string label{};
  };

  std::string formatDuration(std::chrono::milliseconds duration);
  std::string transportLabel(audio::Transport transport);
  bool transportNeedsClockTick(audio::Transport transport);
  QualityIndicatorStyle qualityIndicatorStyle(audio::Quality quality);
  std::string trackDisplayTitle(rt::TrackRow const& row);
  std::string trackDisplayDetail(rt::TrackRow const& row);
  std::string listNodeIcon(rt::ListNodeKind kind);
  std::string listTitle(ListId listId, std::vector<LibraryNavItem> const& items);

  std::vector<LibraryNavItem> makeLibraryNavigation(std::vector<rt::ListNode> const& lists);
  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavItem> const& items);
  TrackListItem makeTrackListItem(rt::TrackRow const& row);
  std::string trackTableLabel(rt::TrackRow const& row);
  std::vector<std::string> menuLabels(std::vector<TrackListItem> const& tracks);
  std::vector<TrackDetailLine> trackDetailLines(rt::TrackRow const& row);
  std::string selectionSummary(std::size_t trackCount, std::int32_t selectedIndex);
  std::int32_t moveSelection(std::int32_t selectedIndex, std::int32_t delta, std::size_t itemCount);
  std::size_t clampSelection(std::size_t selection, std::size_t itemCount);
} // namespace ao::tui
