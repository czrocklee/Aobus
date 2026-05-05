// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#include <cstring>

namespace ao::audio
{
  PcmRingBuffer::PcmRingBuffer()
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
    if (input.empty())
    {
      return 0;
    }

    auto const written = _queue.push(input.data(), input.size());
    _writeCount.fetch_add(written, std::memory_order_release);
    return written;
  }

  std::size_t PcmRingBuffer::read(std::span<std::byte> output) noexcept
  {
    if (output.empty())
    {
      return 0;
    }

    auto const read = _queue.pop(output.data(), output.size());
    _readCount.fetch_add(read, std::memory_order_release);
    return read;
  }

  void PcmRingBuffer::clear() noexcept
  {
    std::byte dummy{};

    while (_queue.pop(dummy))
    {
    }

    _writeCount.store(0, std::memory_order_relaxed);
    _readCount.store(0, std::memory_order_relaxed);
  }
} // namespace ao::audio