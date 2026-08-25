// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio::backend::detail
{
  /** @brief Counts conservative silent presentation frames after a drained render. */
  class AudioBackendDrainTail final
  {
  public:
    bool start(std::uint64_t presentationTailFrames, std::uint64_t silentSuffixFrames) noexcept;
    bool consume(std::uint64_t silentFrames) noexcept;
    void reset() noexcept;

    bool active() const noexcept;
    std::uint64_t remainingFrames() const noexcept;

  private:
    std::uint64_t _remainingFrames = 0U;
    bool _active = false;
  };
} // namespace ao::audio::backend::detail
