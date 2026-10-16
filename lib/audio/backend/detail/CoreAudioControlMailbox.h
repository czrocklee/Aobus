// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace ao::audio::backend::detail
{
  enum class CoreAudioFault : std::uint8_t
  {
    None,
    DeviceLost,
    ChannelRouteChanged,
    RenderBufferShape,
    RenderFrameLimit,
    RenderTargetMissing,
  };

  struct CoreAudioControlFault final
  {
    std::uint64_t generation = 0;
    CoreAudioFault kind = CoreAudioFault::None;
  };

  // Native listeners and rendering may publish after generation admission but
  // before a new run. Older publishers must not erase or block newer work.
  // Fixed lock-free slots coalesce pending work, not already consumed events.
  // Fault generations retain the runtime's existing 56-bit packed representation.
  class CoreAudioControlMailbox final
  {
  public:
    bool tryPublishDeviceChange(std::uint64_t generation) noexcept;
    std::uint64_t takeDeviceChange() noexcept;

    // First cause wins within one pending generation; a newer run supersedes it.
    bool tryPublishFault(std::uint64_t generation, CoreAudioFault fault) noexcept;
    std::optional<CoreAudioControlFault> takeFault() noexcept;

  private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> _pendingDeviceGeneration{0};
    std::atomic<std::uint64_t> _pendingFault{0};
  };
} // namespace ao::audio::backend::detail
