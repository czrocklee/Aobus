// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <span>
#include <vector>

namespace ao::uimodel
{
  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields,
                                                              std::span<rt::TrackField const> storedOrder);
  std::vector<rt::TrackField> visibleTrackFieldsInStoredLayout(std::span<rt::TrackField const> presentationFields,
                                                               std::span<TrackColumnState const> storedLayout);
  std::vector<TrackColumnState> mergeVisibleTrackColumnLayout(std::span<TrackColumnState const> storedLayout,
                                                              std::span<TrackColumnState const> visibleLayout);
} // namespace ao::uimodel
