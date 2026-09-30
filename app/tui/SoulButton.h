// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Transport.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace ao::rt
{
  struct PlaybackTransportSnapshot;
} // namespace ao::rt

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

  /// Advances shared Soul motion from terminal redraws. Redraws stop once the
  /// Soul settles, so the gap before the next one is never counted as motion.
  class SoulAnimationClock final
  {
  public:
    void update(rt::PlaybackTransportSnapshot const& state,
                bool reducedMotion,
                uimodel::FrameClock::TimePoint frameTime) noexcept;

    /// True while a pause coast or aura cross-fade needs redraws that the
    /// transport clock no longer requests.
    bool isSettling() const noexcept;
    uimodel::AobusSoulAnimationState const& animation() const noexcept;

  private:
    uimodel::AobusSoulAnimationState _animation{};
    std::optional<uimodel::FrameClock::TimePoint> _optPreviousFrameTime;
  };
} // namespace ao::tui
