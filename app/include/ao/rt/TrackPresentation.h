// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackField.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr std::string_view kDefaultTrackPresentationId = "library";
  inline constexpr std::string_view kListOrderTrackPresentationId = "list-order";

  struct TrackPresentationSpec final
  {
    std::string id{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackField> visibleFields{};
    std::vector<TrackField> redundantFields{};

    bool operator==(TrackPresentationSpec const&) const = default;
  };

  struct TrackPresentationPreset final
  {
    TrackPresentationSpec spec{};
    std::string_view label{};
    std::string_view description{};
  };

  struct CustomTrackPresentationPreset final
  {
    std::string label{};
    std::string basePresetId{};
    TrackPresentationSpec spec{};

    bool operator==(CustomTrackPresentationPreset const&) const = default;
  };

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets();
  TrackPresentationPreset const* builtinTrackPresentationPreset(std::string_view id);
  TrackPresentationSpec defaultTrackPresentationSpec();
  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec);
} // namespace ao::rt
