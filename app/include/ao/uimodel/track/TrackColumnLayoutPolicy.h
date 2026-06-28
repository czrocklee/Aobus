// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ao::uimodel::track
{
  rt::TrackField expandingTrackColumn(std::span<rt::TrackField const> visibleFields);

  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields,
                                                              std::span<rt::TrackField const> storedOrder);

  std::int32_t effectiveTrackFieldColumnWidth(rt::TrackField field, std::int32_t storedWidth);
} // namespace ao::uimodel::track
