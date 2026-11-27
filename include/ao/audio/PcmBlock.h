// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio
{
  struct PcmBlock final
  {
    /// @brief Borrows decoder-owned PCM until the next readNextBlock() or session destruction.
    /// Ordinary seeks and flushes preserve these bytes. Retire all PCM borrows before calling
    /// DecoderSession::seekAfterDiscard().
    std::span<std::byte const> bytes = {};
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };
} // namespace ao::audio
