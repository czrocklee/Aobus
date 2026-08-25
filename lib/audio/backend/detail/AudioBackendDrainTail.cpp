// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/AudioBackendDrainTail.h"

#include <algorithm>
#include <cstdint>

namespace ao::audio::backend::detail
{
  bool AudioBackendDrainTail::start(std::uint64_t const presentationTailFrames,
                                    std::uint64_t const silentSuffixFrames) noexcept
  {
    _active = true;
    _remainingFrames = presentationTailFrames - std::min(presentationTailFrames, silentSuffixFrames);
    return _remainingFrames == 0U;
  }

  bool AudioBackendDrainTail::consume(std::uint64_t const silentFrames) noexcept
  {
    if (!_active || _remainingFrames == 0U)
    {
      return false;
    }

    _remainingFrames -= std::min(_remainingFrames, silentFrames);
    return _remainingFrames == 0U;
  }

  void AudioBackendDrainTail::reset() noexcept
  {
    _remainingFrames = 0U;
    _active = false;
  }

  bool AudioBackendDrainTail::active() const noexcept
  {
    return _active;
  }

  std::uint64_t AudioBackendDrainTail::remainingFrames() const noexcept
  {
    return _remainingFrames;
  }
} // namespace ao::audio::backend::detail
