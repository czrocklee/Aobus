// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/SeekSliderInteractionModel.h>

#include <algorithm>
#include <chrono>

namespace ao::uimodel::playback
{
  void SeekSliderInteractionModel::applyViewState(std::chrono::milliseconds duration, bool enabled) noexcept
  {
    _duration = duration;
    _enabled = enabled && duration > std::chrono::milliseconds{0};

    if (!_enabled)
    {
      _pointerActive = false;
      _pendingFinalSeek = false;
    }
  }

  void SeekSliderInteractionModel::reset() noexcept
  {
    _duration = std::chrono::milliseconds{0};
    _enabled = false;
    _pointerActive = false;
    _pendingFinalSeek = false;
  }

  bool SeekSliderInteractionModel::beginPointerInteraction() noexcept
  {
    if (!_enabled)
    {
      return false;
    }

    if (!_pointerActive)
    {
      _pendingFinalSeek = false;
    }

    _pointerActive = true;
    return true;
  }

  SeekSliderDecision SeekSliderInteractionModel::endPointerInteraction(std::chrono::milliseconds elapsed) noexcept
  {
    if (!_pointerActive)
    {
      return {};
    }

    _pointerActive = false;

    if (!_pendingFinalSeek)
    {
      return {};
    }

    _pendingFinalSeek = false;
    return {.action = SeekSliderAction::Commit, .elapsed = clampElapsed(elapsed)};
  }

  SeekSliderDecision SeekSliderInteractionModel::valueChanged(std::chrono::milliseconds elapsed) noexcept
  {
    if (!_enabled)
    {
      return {};
    }

    auto const clampedElapsed = clampElapsed(elapsed);

    if (_pointerActive)
    {
      _pendingFinalSeek = true;
      return {.action = SeekSliderAction::Preview, .elapsed = clampedElapsed};
    }

    _pendingFinalSeek = false;
    return {.action = SeekSliderAction::Commit, .elapsed = clampedElapsed};
  }

  std::chrono::milliseconds SeekSliderInteractionModel::clampElapsed(std::chrono::milliseconds elapsed) const noexcept
  {
    auto const upperDuration = _duration > std::chrono::milliseconds{0} ? _duration : std::chrono::milliseconds{0};
    return std::clamp(elapsed, std::chrono::milliseconds{0}, upperDuration);
  }
} // namespace ao::uimodel::playback
