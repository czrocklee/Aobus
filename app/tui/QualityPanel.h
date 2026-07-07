// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct PlaybackState;
} // namespace ao::rt

namespace ao::tui
{
  std::int32_t qualityPanelColumns(rt::PlaybackState const& state, std::int32_t terminalColumns);
  ftxui::Element qualityPanel(rt::PlaybackState const& state, std::int32_t columns = 0);
} // namespace ao::tui
