// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#include <cstddef>
#include <memory>
#include <span>

namespace ao::audio
{
  PcmRingBuffer::PcmRingBuffer()
    : _queuePtr{std::make_unique<Queue>()}
  {
  }

  std::size_t PcmRingBuffer::write(std::span<std::byte const> input) noexcept
  {
    if (input.empty())
    {
      return 0;
    }

    return _queuePtr->push(input.data(), input.size());
  }

  std::size_t PcmRingBuffer::read(std::span<std::byte> output) noexcept
  {
    if (output.empty())
    {
      return 0;
    }

    return _queuePtr->pop(output.data(), output.size());
  }

  void PcmRingBuffer::clear() noexcept
  {
    auto dummy = std::byte{};

    while (_queuePtr->pop(dummy))
    {
    }
  }
} // namespace ao::audio
