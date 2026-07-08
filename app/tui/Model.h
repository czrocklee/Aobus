// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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

  struct TrackSection final
  {
    std::size_t rowBegin = 0;
    std::size_t rowCount = 0;
    std::string primaryText{};
    std::string secondaryText{};
    std::string tertiaryText{};
    ResourceId imageId{kInvalidResourceId};
  };

  struct PresentationNavItem final
  {
    std::string id{};
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
  bool needsTransportClockTick(audio::Transport transport);
  QualityIndicatorStyle qualityIndicatorStyle(uimodel::AudioQualityCategory category);
  QualityIndicatorStyle qualityIndicatorStyle(audio::Quality quality);
  std::string trackDisplayTitle(rt::TrackRow const& row);
  std::string trackDisplayDetail(rt::TrackRow const& row);
  std::string presentationDisplayId(std::string_view presentationId);
  std::string presentationBadgeLabel(std::string_view presentationId);
  std::string sectionDisplayName(TrackSection const& section);
  std::string listNodeIcon(rt::ListNodeKind kind);
  std::string listTitle(ListId listId, std::vector<LibraryNavItem> const& items);

  std::vector<LibraryNavItem> makeLibraryNavigation(std::vector<rt::ListNode> const& lists);
  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavItem> const& items);
  std::vector<PresentationNavItem> makePresentationNavigation(
    std::span<rt::TrackPresentationPreset const> builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> customPresets);
  TrackListItem makeTrackListItem(rt::TrackRow const& row);
  std::string trackTableLabel(rt::TrackRow const& row);
  std::vector<std::string> menuLabels(std::vector<TrackListItem> const& tracks);
  std::vector<TrackDetailLine> trackDetailLines(rt::TrackRow const& row);
  std::string selectionSummary(std::size_t trackCount, std::int32_t selectedIndex);
  std::int32_t moveSelection(std::int32_t selectedIndex, std::int32_t delta, std::size_t itemCount);
  std::size_t clampSelection(std::size_t selection, std::size_t itemCount);
} // namespace ao::tui
