// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
#include <ao/CoreIds.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  ftxui::Element trackTableView(std::vector<TrackListItem> const& tracks,
                                std::int32_t selected,
                                TrackId playingTrackId);
  ftxui::Element libraryChooserPane(std::vector<std::string> const& labels, std::int32_t selected);
} // namespace ao::tui
