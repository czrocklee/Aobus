// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio::backend::detail
{
  /// Reports completion produced by one update, not a persistent drained state.
  enum class DrainTailEvent : std::uint8_t
  {
    None,
    Completed,
  };

  /** @brief Counts conservative silent presentation frames after a drained render. */
  class AudioBackendDrainTail final
  {
  public:
    DrainTailEvent start(std::uint64_t presentationTailFrames, std::uint64_t silentSuffixFrames) noexcept;
    DrainTailEvent consume(std::uint64_t silentFrames) noexcept;
    void reset() noexcept;

    bool isActive() const noexcept;
    std::uint64_t remainingFrames() const noexcept;

  private:
    std::uint64_t _remainingFrames = 0U;
    bool _active = false;
  };
} // namespace ao::audio::backend::detail
