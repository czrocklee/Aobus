// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <memory>
#include <string>

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
  ftxui::Element playbackBar(rt::PlaybackState const& state,
                             std::string const& listTitle,
                             std::chrono::milliseconds displayElapsed);

  ftxui::Element qualityPanel(rt::PlaybackState const& state);
} // namespace ao::tui
