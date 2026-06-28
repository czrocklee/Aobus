// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::uimodel::track
{
  std::int32_t defaultTrackFieldColumnWidth(rt::TrackField field);
  bool trackFieldIsVisibleByDefault(rt::TrackField field);
  std::string_view trackFieldColumnTitle(rt::TrackField field);

  std::optional<rt::TrackField> redundantSortFieldColumn(rt::TrackSortField field);
} // namespace ao::uimodel::track
