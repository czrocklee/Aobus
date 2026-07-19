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
  struct PlaybackTransportSnapshot;
} // namespace ao::rt

namespace ao::tui
{
  std::int32_t qualityPanelColumns(rt::PlaybackTransportSnapshot const& state, std::int32_t terminalColumns);
  ftxui::Element qualityPanel(rt::PlaybackTransportSnapshot const& state, std::int32_t columns = 0);
} // namespace ao::tui
