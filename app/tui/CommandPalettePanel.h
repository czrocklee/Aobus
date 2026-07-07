// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ShellModel.h"

#include <cstdint>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  std::int32_t commandPalettePanelColumns(std::int32_t terminalColumns);
  std::int32_t commandPalettePanelRows(std::int32_t terminalRows);
  ftxui::Element commandPalettePanel(ShellModel const& shell, std::int32_t columns = 0);
} // namespace ao::tui
