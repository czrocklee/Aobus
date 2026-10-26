// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  TrackHotHeader makeMinimalTrackHotHeader();
  std::vector<std::byte> makeMinimalHotTrackViewData();
  std::vector<std::byte> makeHotTrackViewData(std::string_view title);
  std::vector<std::byte> makeColdTrackViewData(TrackColdHeader const& header = {},
                                               std::vector<std::pair<std::string, std::string>> const& customPairs = {},
                                               std::string_view uri = "");
  TrackView makeColdTrackView(std::vector<std::byte> const& data);
} // namespace ao::library::test
