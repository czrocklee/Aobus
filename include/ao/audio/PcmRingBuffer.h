// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <cstddef>
#include <span>

namespace ao::audio
{
  // Capacity in bytes. 2097152 ≈ 0.5s at 192kHz stereo 24-bit (about 1.15MB)
  constexpr std::size_t kRingBufferCapacity = 2097152;

  // Store raw bytes (supports any bitdepth: 16/24/32-bit)
  class PcmRingBuffer final
  {
  public:
    PcmRingBuffer();

    // Write bytes. Returns number of bytes actually written.
    std::size_t write(std::span<std::byte const> input) noexcept;

    // Read bytes into output. Returns bytes actually read.
    std::size_t read(std::span<std::byte> output) noexcept;

    void clear() noexcept;

    // Bytes currently buffered (available to read). The queue tracks this
    // internally, so no separate accounting is kept.
    std::size_t size() const noexcept { return _queue.read_available(); }

    std::size_t capacity() const noexcept { return kRingBufferCapacity; }

  private:
    boost::lockfree::spsc_queue<std::byte, boost::lockfree::capacity<kRingBufferCapacity>> _queue;
  };
} // namespace ao::audio