// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <algorithm>
#include <chrono>

namespace ao::uimodel
{
  void SeekInteraction::applyViewState(std::chrono::milliseconds duration,
                                       bool enabled,
                                       rt::PlaybackOccurrenceId const occurrenceId) noexcept
  {
    _duration = duration;
    _occurrenceId = occurrenceId;
    _enabled = enabled && occurrenceId.value != 0 && duration > std::chrono::milliseconds{0};

    if (_pointerActive && (!_enabled || occurrenceId != _pointerOccurrenceId))
    {
      _pointerOccurrenceCurrent = false;
      _pendingFinalSeek = false;
    }
  }

  void SeekInteraction::reset() noexcept
  {
    _duration = std::chrono::milliseconds{0};
    _occurrenceId = {};
    _pointerDuration = std::chrono::milliseconds{0};
    _pointerOccurrenceId = {};
    _enabled = false;
    _pointerActive = false;
    _pointerOccurrenceCurrent = false;
    _pendingFinalSeek = false;
  }

  bool SeekInteraction::tryBeginPointerInteraction() noexcept
  {
    if (!_enabled)
    {
      return false;
    }

    if (_pointerActive)
    {
      return _pointerOccurrenceCurrent;
    }

    _pendingFinalSeek = false;
    _pointerDuration = _duration;
    _pointerOccurrenceId = _occurrenceId;
    _pointerActive = true;
    _pointerOccurrenceCurrent = true;
    return true;
  }

  SeekSliderUpdate SeekInteraction::endPointerInteraction(std::chrono::milliseconds elapsed) noexcept
  {
    if (!_pointerActive)
    {
      return {};
    }

    auto const clampedElapsed = clampElapsed(elapsed);
    _pointerActive = false;

    if (!_pointerOccurrenceCurrent || !_pendingFinalSeek)
    {
      _pointerOccurrenceCurrent = false;
      _pendingFinalSeek = false;
      return {};
    }

    _pointerOccurrenceCurrent = false;
    _pendingFinalSeek = false;
    return {.action = SeekSliderAction::Commit, .occurrenceId = _pointerOccurrenceId, .elapsed = clampedElapsed};
  }

  SeekSliderUpdate SeekInteraction::valueChanged(std::chrono::milliseconds elapsed) noexcept
  {
    if (_pointerActive)
    {
      if (!_pointerOccurrenceCurrent)
      {
        return {};
      }

      _pendingFinalSeek = true;
      return {
        .action = SeekSliderAction::Preview, .occurrenceId = _pointerOccurrenceId, .elapsed = clampElapsed(elapsed)};
    }

    if (!_enabled)
    {
      return {};
    }

    _pendingFinalSeek = false;
    return {.action = SeekSliderAction::Commit, .occurrenceId = _occurrenceId, .elapsed = clampElapsed(elapsed)};
  }

  std::chrono::milliseconds SeekInteraction::clampElapsed(std::chrono::milliseconds elapsed) const noexcept
  {
    auto const activeDuration = duration();
    auto const upperDuration =
      activeDuration > std::chrono::milliseconds{0} ? activeDuration : std::chrono::milliseconds{0};
    return std::clamp(elapsed, std::chrono::milliseconds{0}, upperDuration);
  }

  std::chrono::milliseconds SeekInteraction::duration() const noexcept
  {
    return _pointerActive ? _pointerDuration : _duration;
  }
} // namespace ao::uimodel
