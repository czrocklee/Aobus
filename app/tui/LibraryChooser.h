// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LibraryNavigation.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::tui
{
  class KeymapPlan;
  class ListSearch;

  inline constexpr std::int32_t kLibraryChooserRows = 14;

  struct LibraryRowHitRegion final
  {
    ListId id = kInvalidListId;
    ftxui::Box box{};
  };

  std::int32_t libraryChooserPaneColumns(i18n::MessageCatalog const& textCatalog,
                                         std::vector<std::string> const& labels,
                                         KeymapPlan const& keymapPlan,
                                         std::int32_t terminalColumns);
  /// Labels and items must have equal sizes and matching row order.
  ftxui::Element libraryChooserPane(i18n::MessageCatalog const& textCatalog,
                                    std::vector<std::string> const& labels,
                                    std::vector<LibraryNavEntry> const& items,
                                    std::int32_t selected,
                                    KeymapPlan const& keymapPlan,
                                    std::int32_t columns,
                                    ListSearch const& search,
                                    std::vector<LibraryRowHitRegion>& rowHitRegions,
                                    ftxui::Box& viewportBox);
} // namespace ao::tui
