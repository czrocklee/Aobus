// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ao::tui
{
  std::int32_t navigationPageRows(ftxui::Box const& viewport);
  std::optional<std::int32_t> listNavigationDelta(ftxui::Event const& event,
                                                  std::int32_t pageRows,
                                                  bool vimKeys = false);
  std::string selectionSummary(i18n::MessageCatalog const& textCatalog,
                               std::size_t trackCount,
                               std::int32_t selectedIndex,
                               std::size_t markedCount = 0,
                               bool visualSelectionActive = false);
  std::int32_t moveSelection(std::int32_t selectedIndex, std::int32_t delta, std::size_t itemCount);
  std::size_t clampSelection(std::size_t selection, std::size_t itemCount);
} // namespace ao::tui
