// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListEntry.h"

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  ftxui::Element renderKittyCoverArtPlaceholder(bool hasCover);
  void paintKittyCoverArt(ftxui::Box const& coverBox, std::vector<std::byte> const& png);

  ftxui::Element centerPopover(ftxui::Element popoverPtr);
  std::int32_t detailPaneColumns(TrackListEntry const* selectedTrack, std::int32_t terminalColumns);
  ftxui::Element detailPane(TrackListEntry const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t columns = 0);
  std::int32_t helpPaneColumns(std::int32_t terminalColumns);
  ftxui::Element helpPane(std::int32_t columns = 0);
} // namespace ao::tui
