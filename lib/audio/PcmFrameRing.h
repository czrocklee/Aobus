// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ao::audio
{
  // 2 MiB holds about 1.82 s of 192 kHz stereo packed 24-bit PCM or 1.37 s at 32-bit.
  constexpr std::size_t kPcmFrameRingByteCapacity = 2097152;

  // Single-producer/single-consumer ring that moves whole PCM frames of any
  // encoding. Every byte count it accepts or reports is a whole number of frames;
  // write() leaves an unwritten tail with the caller. clear() is a control-domain
  // operation and requires exclusive access against all other calls.
  class PcmFrameRing final
  {
  public:
    // Requires 0 < frameByteCount <= kPcmFrameRingByteCapacity.
    explicit PcmFrameRing(std::size_t frameByteCount);

    // Each returns the bytes moved.
    std::size_t write(std::span<std::byte const> input) noexcept;
    std::size_t read(std::span<std::byte> output) noexcept;

    // Reset to empty in constant time. Debug builds diagnose an overlapping call.
    void clear() noexcept;

    // A lower bound for the consumer and the producer respectively; for any
    // other thread an unsynchronized snapshot.
    std::size_t readableByteCount() const noexcept;
    std::size_t writableByteCount() const noexcept;

    std::size_t byteCapacity() const noexcept { return wholeFrameByteCount(kPcmFrameRingByteCapacity); }
    std::size_t frameByteCount() const noexcept { return _frameByteCount; }

  private:
    using Queue = boost::lockfree::spsc_queue<std::byte, boost::lockfree::capacity<kPcmFrameRingByteCapacity>>;

    // The queue embeds its 2 MiB buffer, so it lives on the heap rather than in
    // every enclosing object and stack frame.
    std::unique_ptr<Queue> _queuePtr;
    std::size_t const _frameByteCount;

    // Kept in the object in every configuration for layout stability, but only
    // touched by debug builds so release render/producer paths gain no access
    // accounting. The high bit denotes clear(); the remaining bits count
    // ordinary queue operations.
    mutable std::atomic<std::uint32_t> _debugAccessState{0};

    std::size_t wholeFrameByteCount(std::size_t byteCount) const noexcept
    {
      return byteCount / _frameByteCount * _frameByteCount;
    }

    void beginDebugAccess() const noexcept;
    void endDebugAccess() const noexcept;
    void beginDebugClear() noexcept;
    void endDebugClear() noexcept;
  };
} // namespace ao::audio
