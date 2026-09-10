// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include "PanelWidths.h"
#include "Style.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <vector>

namespace ao::tui
{
  inline constexpr std::int32_t kNavigationColumns = 26;

  class ListNavigationModel;

  struct NavigationGeometry final
  {
    bool canDock = false;
    bool docked = false;
    std::int32_t columns = 0;
    std::int32_t trackColumns = 1;
    std::int32_t detailColumns = 0;
    std::int32_t terminalColumns = 0;
    bool operator==(NavigationGeometry const&) const = default;
  };

  NavigationGeometry navigationGeometry(std::int32_t terminalColumns,
                                        std::int32_t detailColumns,
                                        bool pinned,
                                        bool separateBorders = false,
                                        PanelWidths widths = {});

  struct NavigationRowHit final
  {
    ListId id = kInvalidListId;
    ftxui::Box row = kEmptyMouseBox;
    ftxui::Box disclosure = kEmptyMouseBox;
  };

  struct NavigationHitRegions final
  {
    PanelMouseRegions panel{};
    std::uint64_t revision = 0;
    std::vector<NavigationRowHit> rows{};
  };

  struct NavigationPanelOptions final
  {
    std::int32_t columns = kNavigationColumns;
    bool focused = false;
    NavigationHitRegions* regions = nullptr;
  };

  /// Joins framed Lists and workspace panes using shared or separate borders.
  ftxui::Element dockNavigationPanel(ftxui::Element navigationPtr,
                                     ftxui::Element workspacePtr,
                                     std::int32_t navigationColumns,
                                     ftxui::Box* pinBox = nullptr,
                                     bool hovered = false,
                                     style::PanelDividerOptions options = {});

  ftxui::Element collapsedNavigationPanel(ftxui::Element workspacePtr,
                                          ftxui::Box& pinBox,
                                          bool hovered = false,
                                          bool revealOnHover = false,
                                          ftxui::Box* hoverBox = nullptr);

  ftxui::Element navigationPanel(i18n::MessageCatalog const& catalog,
                                 ListNavigationModel const& model,
                                 ListId activeList,
                                 NavigationPanelOptions options);
} // namespace ao::tui
