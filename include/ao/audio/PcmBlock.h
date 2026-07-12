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
    /// @brief Aliasing span of decoder-owned memory; valid until next readNextBlock() or decoder close.
    std::span<std::byte const> bytes = {};
    std::uint8_t bitDepth = 16;
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };
} // namespace ao::audio
