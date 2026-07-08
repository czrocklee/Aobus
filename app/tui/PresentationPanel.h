// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentationNavigation.h"

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  inline constexpr std::int32_t kPresentationPanelColumns = 48;
  inline constexpr std::int32_t kPresentationPanelListRows = 10;
  inline constexpr std::int32_t kPresentationPanelChromeRows = 6;
  inline constexpr std::int32_t kPresentationPanelRows = kPresentationPanelListRows + kPresentationPanelChromeRows;

  struct PresentationRowHitRegion final
  {
    std::int32_t rowIndex = -1;
    ftxui::Box box{};
  };

  std::int32_t presentationPanelColumns(std::vector<TrackPresentationNavEntry> const& items,
                                        std::string_view activePresentationId,
                                        std::int32_t terminalColumns);
  ftxui::Element presentationPanel(std::vector<TrackPresentationNavEntry> const& items,
                                   std::string_view activePresentationId,
                                   std::int32_t selectedIndex,
                                   std::vector<PresentationRowHitRegion>* rowHitRegions = nullptr,
                                   std::int32_t columns = 0);
} // namespace ao::tui
