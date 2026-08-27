// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
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

  std::string trackPresentationDisplayId(i18n::MessageCatalog const& textCatalog, std::string_view presentationId);
  std::string trackPresentationBadgeLabel(i18n::MessageCatalog const& textCatalog, std::string_view presentationId);
  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    i18n::MessageCatalog const& textCatalog,
    std::span<rt::TrackPresentationPreset const> builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::tui
