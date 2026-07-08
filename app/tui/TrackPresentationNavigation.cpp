// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPresentationNavigation.h"

#include <ao/rt/TrackPresentation.h>

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    auto items = std::vector<TrackPresentationNavEntry>{};
    items.reserve(builtinPresets.size() + customPresets.size());

    for (auto const& preset : builtinPresets)
    {
      items.push_back(TrackPresentationNavEntry{
        .id = preset.spec.id,
        .label = preset.label.empty() ? preset.spec.id : std::string{preset.label},
        .detail = std::string{preset.description},
      });
    }

    for (auto const& preset : customPresets)
    {
      items.push_back(TrackPresentationNavEntry{
        .id = preset.spec.id,
        .label = preset.label.empty() ? preset.spec.id : preset.label,
        .detail =
          preset.basePresetId.empty() ? std::string{"custom"} : std::format("custom from {}", preset.basePresetId),
      });
    }

    return items;
  }

  std::string trackPresentationDisplayId(std::string_view const presentationId)
  {
    return presentationId.empty() ? std::string{"default"} : std::string{presentationId};
  }

  std::string trackPresentationBadgeLabel(std::string_view const presentationId)
  {
    return std::format("view:{}", trackPresentationDisplayId(presentationId));
  }
} // namespace ao::tui
