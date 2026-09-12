// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Transport.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <chrono>
#include <memory>
#include <string>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  /// Projects the shared three-cell Soul frame into unstyled title text.
  std::string soulTitleText(audio::Transport transport,
                            uimodel::AobusSoulMotionFrame const& motion,
                            std::chrono::milliseconds transientElapsed);

  ftxui::Element soulButtonElement(audio::Transport transport,
                                   uimodel::AobusSoulVisualFrame const& visual,
                                   std::chrono::milliseconds transientElapsed);
} // namespace ao::tui
