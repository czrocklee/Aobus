// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackPresentation.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  struct TrackPresentationNavEntry final
  {
    std::string id{};
    std::string label{};
    std::string detail{};
  };

  std::string trackPresentationDisplayId(std::string_view presentationId);
  std::string trackPresentationBadgeLabel(std::string_view presentationId);
  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    std::span<rt::TrackPresentationPreset const> builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::tui
