// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "StateTypes.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  struct TrackPresentationSpec final
  {
    std::string id{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackPresentationField> visibleFields{};
    std::vector<TrackPresentationField> redundantFields{};

    bool operator==(TrackPresentationSpec const&) const = default;
  };

  struct TrackPresentationPreset final
  {
    TrackPresentationSpec spec{};
    std::string_view label{};
    std::string_view description{};
  };

  std::string_view trackPresentationFieldId(TrackPresentationField field);
  std::optional<TrackPresentationField> trackPresentationFieldFromId(std::string_view id);

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets();
  TrackPresentationPreset const* builtinTrackPresentationPreset(std::string_view id);
  TrackPresentationSpec defaultTrackPresentationSpec();
  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec);

  TrackListPresentationState presentationStateFromSpec(TrackPresentationSpec const& spec);
  TrackPresentationSpec presentationSpecFromState(TrackListPresentationState const& state);
} // namespace ao::rt
