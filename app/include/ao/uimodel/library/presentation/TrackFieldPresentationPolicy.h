// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  enum class TrackColumnSizing : std::uint8_t
  {
    Fixed,
    Flexible,
  };

  enum class TrackColumnAlignment : std::uint8_t
  {
    Start,
    End,
  };

  std::int32_t defaultTrackFieldColumnWidth(rt::TrackField field);
  std::int32_t minimumTrackFieldColumnWidth(rt::TrackField field);
  double defaultTrackFieldColumnWeight(rt::TrackField field);
  TrackColumnSizing trackFieldColumnSizing(rt::TrackField field);
  TrackColumnAlignment trackFieldColumnAlignment(rt::TrackField field);

  bool isTrackFieldVisibleByDefault(rt::TrackField field);
  std::string_view trackFieldColumnTitle(rt::TrackField field);

  std::optional<rt::TrackField> redundantSortFieldColumn(rt::TrackSortField field);
} // namespace ao::uimodel
