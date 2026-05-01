// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace rs::audio
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

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept { return kRingBufferCapacity; }

  private:
    boost::lockfree::spsc_queue<std::byte, boost::lockfree::capacity<kRingBufferCapacity>> _queue;
    mutable std::mutex _mutex;
    std::atomic<std::size_t> _writeCount{0};
    std::atomic<std::size_t> _readCount{0};
  };

} // namespace rs::audio