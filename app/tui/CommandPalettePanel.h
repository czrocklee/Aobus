// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Keymap.h"
#include "MouseBindings.h"
#include "ShellInteractionModel.h"
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  struct CompletionHitRegions final
  {
    std::string draft{};
    std::size_t cursor = 0;
    ftxui::Box listBox = kEmptyMouseBox;
    ftxui::Box inputBox = kEmptyMouseBox;
    ftxui::Box inputOrigin = kEmptyMouseBox;
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    std::vector<std::string> insertions{};
    std::vector<ftxui::Box> rows{};
  };

  std::int32_t commandPalettePanelColumns(std::int32_t terminalColumns);
  std::int32_t commandPalettePanelRows(std::int32_t terminalRows);
  std::int32_t quickFilterPanelRows(ShellInteractionModel const& shell, bool hasFilterError, std::int32_t terminalRows);
  ftxui::Element commandPalettePanel(i18n::MessageCatalog const& textCatalog,
                                     ShellInteractionModel const& shell,
                                     KeymapPlan const& keymapPlan,
                                     std::int32_t columns = 0,
                                     CompletionHitRegions* hitRegions = nullptr);
  ftxui::Element quickFilterCompletionPanel(i18n::MessageCatalog const& textCatalog,
                                            ShellInteractionModel const& shell,
                                            KeymapPlan const& keymapPlan,
                                            std::int32_t columns = 0,
                                            std::string_view filterError = {},
                                            CompletionHitRegions* hitRegions = nullptr);
} // namespace ao::tui
