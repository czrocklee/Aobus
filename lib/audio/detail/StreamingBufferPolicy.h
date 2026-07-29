// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ao::audio::detail
{
  std::size_t bufferByteCountForDuration(std::uint64_t bytesPerSecond,
                                         std::chrono::milliseconds duration,
                                         std::size_t capacity) noexcept;

  bool permitsDecode(std::size_t targetByteCount,
                     std::size_t bufferedByteCount,
                     std::size_t writableByteCount,
                     std::size_t previousBlockByteCount) noexcept;
} // namespace ao::audio::detail
