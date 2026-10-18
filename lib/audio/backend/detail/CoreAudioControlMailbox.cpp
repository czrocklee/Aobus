// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioControlMailbox.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace ao::audio::backend::detail
{
  namespace
  {
    constexpr std::uint64_t kFaultKindMask = 0xFFU;
  } // namespace

  bool CoreAudioControlMailbox::tryPublishDeviceChange(std::uint64_t const generation) noexcept
  {
    auto observed = _pendingDeviceGeneration.load(std::memory_order_relaxed);

    while (observed < generation)
    {
      if (_pendingDeviceGeneration.compare_exchange_weak(
            observed, generation, std::memory_order_release, std::memory_order_relaxed))
      {
        return true;
      }
    }

    return false;
  }

  std::uint64_t CoreAudioControlMailbox::takeDeviceChange() noexcept
  {
    return _pendingDeviceGeneration.exchange(0, std::memory_order_acq_rel);
  }

  bool CoreAudioControlMailbox::tryPublishFault(std::uint64_t const generation, CoreAudioFault const fault) noexcept
  {
    auto const encoded = (generation << 8U) | static_cast<std::uint8_t>(fault);
    auto observed = _pendingFault.load(std::memory_order_relaxed);

    while (observed == 0 || (observed >> 8U) < generation)
    {
      if (_pendingFault.compare_exchange_weak(observed, encoded, std::memory_order_release, std::memory_order_relaxed))
      {
        return true;
      }
    }

    return false;
  }

  std::optional<CoreAudioControlFault> CoreAudioControlMailbox::takeFault() noexcept
  {
    auto const encoded = _pendingFault.exchange(0, std::memory_order_acq_rel);

    if (encoded == 0)
    {
      return std::nullopt;
    }

    return CoreAudioControlFault{
      .generation = encoded >> 8U, .kind = static_cast<CoreAudioFault>(encoded & kFaultKindMask)};
  }
} // namespace ao::audio::backend::detail
