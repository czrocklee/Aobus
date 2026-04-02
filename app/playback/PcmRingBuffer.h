// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace app::playback
{

  // Capacity in samples (interleaved). 524288 ≈ 0.5s at 192kHz stereo
  constexpr std::size_t kRingBufferCapacity = 524288;

  // Store S16 samples directly (16-bit interleaved PCM)
  class PcmRingBuffer final
  {
  public:
    PcmRingBuffer();

    // Write samples. Returns number of samples actually written.
    std::size_t write(std::span<std::int16_t const> input) noexcept;

    // Read samples into output. Returns samples actually read.
    std::size_t read(std::span<std::int16_t> output) noexcept;

    void clear() noexcept;

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept { return kRingBufferCapacity; }

  private:
    boost::lockfree::spsc_queue<std::int16_t, boost::lockfree::capacity<kRingBufferCapacity>> _queue;
    mutable std::mutex _mutex;
    std::atomic<std::size_t> _writeCount{0};
    std::atomic<std::size_t> _readCount{0};
  };

} // namespace app::playback