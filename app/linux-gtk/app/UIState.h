// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  constexpr std::int32_t kDefaultWindowWidth = 989;
  constexpr std::int32_t kDefaultWindowHeight = 801;
  constexpr std::int32_t kDefaultPanedPosition = 330;

  struct WindowState final
  {
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;
    std::int32_t panedPosition = kDefaultPanedPosition;
  };

  struct TrackViewState final
  {
    std::string activePresentationId = "songs";

    std::vector<std::string> columnOrder;
    std::vector<std::string> hiddenColumns;
    std::map<std::string, std::int32_t, std::less<>> columnWidths;
  };

  struct TrackPresentationSortTermState final
  {
    std::uint8_t field = 0; // ao::rt::TrackSortField
    bool ascending = true;
  };

  struct CustomTrackPresentationState final
  {
    std::string id;
    std::string label;
    std::string basePresetId;
    std::uint8_t groupBy = 0; // ao::rt::TrackGroupKey
    std::vector<TrackPresentationSortTermState> sortBy;
    std::vector<std::uint8_t> visibleFields;   // ao::rt::TrackPresentationField
    std::vector<std::uint8_t> redundantFields; // ao::rt::TrackPresentationField
  };

  struct TrackPresentationStoreState final
  {
    std::vector<CustomTrackPresentationState> customPresentations;
  };
} // namespace ao::gtk
