// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Transport.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <chrono>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  ftxui::Element soulButtonElement(audio::Transport transport,
                                   uimodel::AobusSoulRgb aura,
                                   std::chrono::milliseconds animationElapsed);
} // namespace ao::tui
