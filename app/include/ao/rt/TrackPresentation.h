// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackField.h"
#include <ao/utility/StrongType.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::rt
{
  enum class BuiltinPreset : std::uint8_t
  {
    Library,
    ListOrder,
    Songs,
    Albums,
    Artists,
    Performers,
    Genres,
    Years,
    ClassicalComposers,
    ClassicalConductors,
    ClassicalWorks,
    Tagging,
    Technical,
  };

  using CustomPresetId = utility::StrongType<std::uint64_t, struct CustomPresetIdTag>;
  using TrackPresetId = std::variant<BuiltinPreset, CustomPresetId>;

  /// Stable keys for storage, command arguments and other text boundaries.
  std::string_view builtinPresetKey(BuiltinPreset id);
  std::string trackPresetKey(TrackPresetId const& id);
  std::optional<TrackPresetId> parseTrackPresetId(std::string_view key) noexcept;

  /// Keep the clicked table column distinct from the comparison key it projects onto.
  struct TrackColumnSort final
  {
    TrackField column = TrackField::Title;
    bool ascending = true;

    bool operator==(TrackColumnSort const&) const = default;
  };

  struct TrackPresentationSpec final
  {
    TrackPresetId presetId = BuiltinPreset::Library;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::optional<TrackColumnSort> optColumnSort{};
    std::vector<TrackField> visibleFields{};
    std::vector<TrackField> redundantFields{};

    TrackColumnSort nextColumnSort(TrackField column) const noexcept;
    /// The selected column sorts first; the preset's remaining keys break ties.
    std::vector<TrackSortTerm> effectiveSortBy() const;
    /// Compares the specs themselves, independent of effective ordering.
    bool hasOnlyColumnSortChange(TrackPresentationSpec const& other) const;
    bool isPreset(TrackPresetId const& id) const noexcept { return presetId == id; }
    bool operator==(TrackPresentationSpec const&) const = default;
  };

  struct TrackPresentationPreset final
  {
    TrackPresentationSpec spec{};

    BuiltinPreset presetId() const { return std::get<BuiltinPreset>(spec.presetId); }
  };

  struct CustomTrackPresentationPreset final
  {
    std::string label{};
    std::optional<TrackPresetId> optBasePresetId{};
    TrackPresentationSpec spec{};

    CustomPresetId presetId() const { return std::get<CustomPresetId>(spec.presetId); }
    bool operator==(CustomTrackPresentationPreset const&) const = default;
  };

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets();
  TrackPresentationPreset const& builtinTrackPresentationPreset(BuiltinPreset id);
  TrackPresentationSpec defaultTrackPresentationSpec();
  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec);

  /// Fields that define a section, in their fallback comparison order.
  std::span<TrackSortField const> trackGroupSortFields(TrackGroupKey groupBy) noexcept;
  /// Section order comes from the base sort, not from a column override.
  std::vector<TrackSortTerm> trackGroupSortBy(TrackGroupKey groupBy, std::span<TrackSortTerm const> baseSortBy);
} // namespace ao::rt
