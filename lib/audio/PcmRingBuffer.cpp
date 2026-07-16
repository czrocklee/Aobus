// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#ifndef NDEBUG
#include <gsl-lite/gsl-lite.hpp>

#include <atomic>
#include <cstdint>
#endif

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

  PcmRingBuffer::PcmRingBuffer()
    : _queuePtr{std::make_unique<Queue>()}
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const written = input.empty() ? 0 : _queuePtr->push(input.data(), input.size());

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

    auto const bytesRead = output.empty() ? 0 : _queuePtr->pop(output.data(), output.size());

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

    auto const available = _queuePtr->write_available();

#ifndef NDEBUG
    endDebugAccess();
#endif

    return available;
  }

#ifndef NDEBUG
  void PcmRingBuffer::beginDebugAccess() const noexcept
  {
    auto observed = _debugAccessState.load(std::memory_order_relaxed);

    while (true)
    {
      gsl_Expects((observed & kDebugClearActive) == 0);
      gsl_Expects((observed & kDebugAccessCountMask) != kDebugAccessCountMask);

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
    gsl_Expects(previous != 0 && (previous & kDebugClearActive) == 0);
  }

  void PcmRingBuffer::beginDebugClear() noexcept
  {
    std::uint32_t expected = 0;
    auto const acquired = _debugAccessState.compare_exchange_strong(
      expected, kDebugClearActive, std::memory_order_acquire, std::memory_order_relaxed);
    gsl_Expects(acquired);
  }

  void PcmRingBuffer::endDebugClear() noexcept
  {
    std::uint32_t expected = kDebugClearActive;
    auto const released = _debugAccessState.compare_exchange_strong(
      expected, std::uint32_t{0}, std::memory_order_release, std::memory_order_relaxed);
    gsl_Expects(released);
  }
#endif
} // namespace ao::audio
