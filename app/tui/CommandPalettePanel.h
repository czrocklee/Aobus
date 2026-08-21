// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ShellInteractionModel.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace ao::uimodel
{
  class PresentationTextCatalog;
}

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  class TuiTextCatalog;
  std::int32_t commandPalettePanelColumns(std::int32_t terminalColumns);
  std::int32_t commandPalettePanelRows(std::int32_t terminalRows);
  std::int32_t quickFilterPanelRows(ShellInteractionModel const& shell, bool hasFilterError, std::int32_t terminalRows);
  ftxui::Element commandPalettePanel(uimodel::PresentationTextCatalog const& textCatalog,
                                     TuiTextCatalog const& tuiTextCatalog,
                                     ShellInteractionModel const& shell,
                                     std::int32_t columns = 0);
  ftxui::Element quickFilterCompletionPanel(uimodel::PresentationTextCatalog const& textCatalog,
                                            TuiTextCatalog const& tuiTextCatalog,
                                            ShellInteractionModel const& shell,
                                            std::int32_t columns = 0,
                                            std::string_view filterError = {});
} // namespace ao::tui
