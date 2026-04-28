// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/source/PcmRingBuffer.h"

#include <cstring>

namespace app::core::source
{

  PcmRingBuffer::PcmRingBuffer()
    : _queue{}
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
    if (input.empty()) return 0;

    auto lock = std::lock_guard<std::mutex>{_mutex};
    auto const written = _queue.push(reinterpret_cast<std::uint8_t const*>(input.data()), input.size());
    _writeCount.fetch_add(written, std::memory_order_release);
    return written;
  }

  std::size_t PcmRingBuffer::read(std::span<std::byte> output) noexcept
  {
    if (output.empty()) return 0;

    auto lock = std::lock_guard<std::mutex>{_mutex};
    auto const read = _queue.pop(reinterpret_cast<std::uint8_t*>(output.data()), output.size());
    _readCount.fetch_add(read, std::memory_order_release);
    return read;
  }

  void PcmRingBuffer::clear() noexcept
  {
    auto lock = std::lock_guard<std::mutex>{_mutex};
    std::uint8_t dummy;

    while (_queue.pop(dummy));

    _writeCount.store(0, std::memory_order_relaxed);
    _readCount.store(0, std::memory_order_relaxed);
  }

  std::size_t PcmRingBuffer::size() const noexcept
  {
    return _writeCount.load(std::memory_order_acquire) - _readCount.load(std::memory_order_acquire);
  }

} // namespace app::core::playback