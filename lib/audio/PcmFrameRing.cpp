// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PcmFrameRing.h"

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

  PcmFrameRing::PcmFrameRing(std::size_t frameByteCount)
    : _queuePtr{std::make_unique<Queue>()}, _frameByteCount{frameByteCount}
  {
    AO_EXPECTS(frameByteCount > 0, "PCM frame ring frame size must be nonzero");
    AO_EXPECTS(frameByteCount <= kPcmFrameRingByteCapacity, "PCM frame ring frame size must fit its capacity");
  }

  std::size_t PcmFrameRing::write(std::span<std::byte const> input) noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const byteCount = wholeFrameByteCount(std::min(input.size(), _queuePtr->write_available()));
    auto const written = byteCount == 0 ? 0 : _queuePtr->push(input.data(), byteCount);

#ifndef NDEBUG
    endDebugAccess();
#endif

    AO_ENSURES(written == byteCount, "PCM frame ring write must commit every accepted frame");
    return written;
  }

  std::size_t PcmFrameRing::read(std::span<std::byte> output) noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const byteCount = wholeFrameByteCount(std::min(output.size(), _queuePtr->read_available()));
    auto const bytesRead = byteCount == 0 ? 0 : _queuePtr->pop(output.data(), byteCount);

#ifndef NDEBUG
    endDebugAccess();
#endif

    AO_RT_INVARIANT(bytesRead == byteCount, "PCM frame ring read must commit every requested frame");
    return bytesRead;
  }

  void PcmFrameRing::clear() noexcept
  {
#ifndef NDEBUG
    beginDebugClear();
#endif

    (*_queuePtr).reset();

#ifndef NDEBUG
    endDebugClear();
#endif
  }

  std::size_t PcmFrameRing::readableByteCount() const noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const byteCount = wholeFrameByteCount(_queuePtr->read_available());

#ifndef NDEBUG
    endDebugAccess();
#endif

    return byteCount;
  }

  std::size_t PcmFrameRing::writableByteCount() const noexcept
  {
#ifndef NDEBUG
    beginDebugAccess();
#endif

    auto const byteCount = wholeFrameByteCount(_queuePtr->write_available());

#ifndef NDEBUG
    endDebugAccess();
#endif

    return byteCount;
  }

#ifndef NDEBUG
  void PcmFrameRing::beginDebugAccess() const noexcept
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

  void PcmFrameRing::endDebugAccess() const noexcept
  {
    auto const previous = _debugAccessState.fetch_sub(1, std::memory_order_release);
    AO_EXPECTS(previous != 0 && (previous & kDebugClearActive) == 0);
  }

  void PcmFrameRing::beginDebugClear() noexcept
  {
    std::uint32_t expected = 0;
    auto const acquired = _debugAccessState.compare_exchange_strong(
      expected, kDebugClearActive, std::memory_order_acquire, std::memory_order_relaxed);
    AO_EXPECTS(acquired);
  }

  void PcmFrameRing::endDebugClear() noexcept
  {
    std::uint32_t expected = kDebugClearActive;
    auto const released = _debugAccessState.compare_exchange_strong(
      expected, std::uint32_t{0}, std::memory_order_release, std::memory_order_relaxed);
    AO_EXPECTS(released);
  }
#endif
} // namespace ao::audio
