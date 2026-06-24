// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#include <cstddef>
#include <span>

namespace ao::audio
{
  PcmRingBuffer::PcmRingBuffer() = default;

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
    if (input.empty())
    {
      return 0;
    }

    return _queue.push(input.data(), input.size());
  }

  std::size_t PcmRingBuffer::read(std::span<std::byte> output) noexcept
  {
    if (output.empty())
    {
      return 0;
    }

    return _queue.pop(output.data(), output.size());
  }

  void PcmRingBuffer::clear() noexcept
  {
    auto dummy = std::byte{};

    while (_queue.pop(dummy))
    {
    }
  }
} // namespace ao::audio