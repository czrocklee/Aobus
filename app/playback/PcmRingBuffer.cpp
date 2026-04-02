// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PcmRingBuffer.h"

namespace app::playback
{

  PcmRingBuffer::PcmRingBuffer()
    : _queue()
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::int16_t const> input) noexcept
  {
    if (input.empty()) { return 0; }

    std::lock_guard<std::mutex> lock(_mutex);
    auto const written = _queue.push(input.data(), input.size());
    _writeCount.fetch_add(written, std::memory_order_release);
    return written;
  }

  std::size_t PcmRingBuffer::read(std::span<std::int16_t> output) noexcept
  {
    if (output.empty()) { return 0; }

    std::lock_guard<std::mutex> lock(_mutex);
    auto const read = _queue.pop(output.data(), output.size());
    _readCount.fetch_add(read, std::memory_order_release);
    return read;
  }

  void PcmRingBuffer::clear() noexcept
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::int16_t dummy{};
    while (_queue.pop(dummy)) { }
    _writeCount.store(0, std::memory_order_relaxed);
    _readCount.store(0, std::memory_order_relaxed);
  }

  std::size_t PcmRingBuffer::size() const noexcept
  {
    return _writeCount.load(std::memory_order_acquire) - _readCount.load(std::memory_order_acquire);
  }

} // namespace app::playback