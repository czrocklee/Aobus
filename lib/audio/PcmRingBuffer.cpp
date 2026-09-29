// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PcmRingBuffer.h"

#include <ao/Contract.h>

#ifndef NDEBUG
#include <atomic>
#include <cstdint>
#endif

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>

namespace ao::audio
{
#ifndef NDEBUG
  namespace
  {
    constexpr std::uint32_t kDebugClearActive = std::uint32_t{1} << 31;
    constexpr std::uint32_t kDebugAccessCountMask = kDebugClearActive - 1;
  } // namespace
#endif

  namespace
  {
    // Runs inside the constructor's member initializer, before the members that
    // divide by the frame size are initialized.
    std::size_t frameCapacityFor(std::size_t frameSize)
    {
      AO_EXPECTS(frameSize > 0, "PCM ring frame size must be nonzero");
      AO_EXPECTS(frameSize <= kRingBufferCapacity, "PCM ring frame size must fit the ring capacity");
      return kRingBufferCapacity / frameSize;
    }
  } // namespace

  PcmRingBuffer::PcmRingBuffer(std::size_t frameSize)
    : _queuePtr{std::make_unique<Queue>()}, _frameSize{frameSize}, _frameCapacity{frameCapacityFor(frameSize)}
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    // The sole producer can only observe writable space grow or stay equal,
    // because only the consumer frees it, so pushing this floored amount always
    // commits fully and never splits a frame across the ring tail.
    auto const writableFrames = _queuePtr->write_available() / _frameSize;
    auto const requestedFrames = std::min(input.size() / _frameSize, writableFrames);
    auto const written = requestedFrames == 0 ? 0 : _queuePtr->push(input.data(), requestedFrames * _frameSize);

#ifndef NDEBUG
    endDebugAccess();
#endif

    return written;
  }

  std::size_t PcmRingBuffer::read(std::span<std::byte> output) noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    // The sole consumer can only observe content grow or stay equal, because
    // only the producer fills it, so popping this floored amount always commits
    // fully. A sub-frame fragment stays buffered instead of escaping the ring
    // as the partial read the RenderTarget contract forbids.
    auto const readableFrames = _queuePtr->read_available() / _frameSize;
    auto const requestedFrames = std::min(output.size() / _frameSize, readableFrames);
    auto const bytesRead = requestedFrames == 0 ? 0 : _queuePtr->pop(output.data(), requestedFrames * _frameSize);

#ifndef NDEBUG
    endDebugAccess();
#endif

    return bytesRead;
  }

  void PcmRingBuffer::clear() noexcept
  {
#ifndef NDEBUG
    beginDebugClear();
#endif

    (*_queuePtr).reset();

#ifndef NDEBUG
    endDebugClear();
#endif

    // Emptiness satisfies the frame invariant.
  }

  std::size_t PcmRingBuffer::size() const noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const size = _queuePtr->read_available();

#ifndef NDEBUG
    endDebugAccess();
#endif

    return size;
  }

  std::size_t PcmRingBuffer::availableToWrite() const noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    // Report whole-frame writable bytes only: the frame-aware producer parks
    // anything that cannot fit as complete frames.
    auto const availableFrames = _queuePtr->write_available() / _frameSize;

#ifndef NDEBUG
    endDebugAccess();
#endif

    return availableFrames * _frameSize;
  }

#ifndef NDEBUG
  void PcmRingBuffer::beginDebugAccess() const noexcept
  {
    auto observed = _debugAccessState.load(std::memory_order_relaxed);

    while (true)
    {
      AO_EXPECTS((observed & kDebugClearActive) == 0);
      AO_EXPECTS((observed & kDebugAccessCountMask) != kDebugAccessCountMask);

      if (_debugAccessState.compare_exchange_weak(
            observed, observed + 1, std::memory_order_acquire, std::memory_order_relaxed))
      {
        return;
      }
    }
  }

  void PcmRingBuffer::endDebugAccess() const noexcept
  {
    auto const previous = _debugAccessState.fetch_sub(1, std::memory_order_release);
    AO_EXPECTS(previous != 0 && (previous & kDebugClearActive) == 0);
  }

  void PcmRingBuffer::beginDebugClear() noexcept
  {
    std::uint32_t expected = 0;
    auto const acquired = _debugAccessState.compare_exchange_strong(
      expected, kDebugClearActive, std::memory_order_acquire, std::memory_order_relaxed);
    AO_EXPECTS(acquired);
  }

  void PcmRingBuffer::endDebugClear() noexcept
  {
    std::uint32_t expected = kDebugClearActive;
    auto const released = _debugAccessState.compare_exchange_strong(
      expected, std::uint32_t{0}, std::memory_order_release, std::memory_order_relaxed);
    AO_EXPECTS(released);
  }
#endif
} // namespace ao::audio
