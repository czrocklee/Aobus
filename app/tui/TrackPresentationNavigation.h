// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>

#include <span>
#include <string>
#include <vector>

namespace ao::tui
{
  struct TrackPresentationNavEntry final
  {
    std::string id{};
    std::string label{};
    std::string detail{}; // Description, or the reason this choice is unavailable.
    bool enabled = true;
  };

  std::string trackPresentationDisplayLabel(i18n::MessageCatalog const& textCatalog,
                                            rt::TrackPresetId const& presetId,
                                            std::span<TrackPresentationNavEntry const> entries);
  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    i18n::MessageCatalog const& textCatalog,
    ListId listId,
    std::span<rt::TrackPresentationPreset const> builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::tui
