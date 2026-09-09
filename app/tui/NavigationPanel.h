// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <vector>

namespace ao::tui
{
  inline constexpr std::int32_t kNavigationColumns = 26;

  class KeymapPlan;
  class ListNavigationModel;

  struct NavigationGeometry final
  {
    bool canDock = false;
    bool docked = false;
    bool drawer = false;
    std::int32_t columns = 0;
    std::int32_t trackColumns = 1;
    bool operator==(NavigationGeometry const&) const = default;
  };

  NavigationGeometry navigationGeometry(std::int32_t terminalColumns,
                                        std::int32_t detailColumns,
                                        bool enabled,
                                        bool focused,
                                        bool suspended);

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

  ftxui::Element navigationPanel(i18n::MessageCatalog const& catalog,
                                 ListNavigationModel const& model,
                                 ListId activeList,
                                 KeymapPlan const& keymap,
                                 NavigationPanelOptions options);
} // namespace ao::tui
