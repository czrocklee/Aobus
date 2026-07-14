// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace ao::audio
{
  // Capacity in bytes. 2 MiB holds about 1.82 s of 192 kHz stereo packed
  // 24-bit PCM or 1.37 s at 32-bit.
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
    std::size_t size() const noexcept { return _queuePtr->read_available(); }

    // Bytes the producer can write without a partial write. This is an
    // advisory SPSC snapshot: the consumer can only increase it.
    std::size_t availableToWrite() const noexcept { return _queuePtr->write_available(); }

    std::size_t capacity() const noexcept { return kRingBufferCapacity; }

  private:
    using Queue = boost::lockfree::spsc_queue<std::byte, boost::lockfree::capacity<kRingBufferCapacity>>;

    // The queue embeds its 2MB buffer, which is far too large for a by-value
    // member (stack frames and enclosing objects would inherit it), so it
    // lives on the heap.
    std::unique_ptr<Queue> _queuePtr;
  };
} // namespace ao::audio
