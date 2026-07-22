// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>

namespace ao::uimodel
{
  enum class SeekSliderAction : std::uint8_t
  {
    None,
    Preview,
    Commit,
  };

  struct SeekSliderUpdate final
  {
    SeekSliderAction action = SeekSliderAction::None;
    std::chrono::milliseconds elapsed{0};
  };

  class SeekSliderInteractionModel final
  {
  public:
    void applyViewState(std::chrono::milliseconds duration, bool enabled) noexcept;
    void reset() noexcept;

    bool beginPointerInteraction() noexcept;
    SeekSliderUpdate endPointerInteraction(std::chrono::milliseconds elapsed) noexcept;
    SeekSliderUpdate valueChanged(std::chrono::milliseconds elapsed) noexcept;

    bool isPointerActive() const noexcept { return _pointerActive; }
    bool hasPendingFinalSeek() const noexcept { return _pendingFinalSeek; }
    std::chrono::milliseconds duration() const noexcept { return _duration; }

  private:
    std::chrono::milliseconds clampElapsed(std::chrono::milliseconds elapsed) const noexcept;

    std::chrono::milliseconds _duration{0};
    bool _enabled = false;
    bool _pointerActive = false;
    bool _pendingFinalSeek = false;
  };
} // namespace ao::uimodel
