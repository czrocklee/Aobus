// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/MemorySource.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::size_t frameBytes(Format const& format) noexcept
    {
      if (format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      std::uint32_t bytesPerSample = 2U;
      if (format.bitDepth == 24U)
      {
        bytesPerSample = kBytesPer24BitSample;
      }
      else if (format.bitDepth > 16U)
      {
        bytesPerSample = 4U;
      }

      return static_cast<std::size_t>(format.channels) * bytesPerSample;
    }

    std::uint64_t bytesPerSecond(Format const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      return static_cast<std::uint64_t>(format.sampleRate) * frameBytes(format);
    }

    std::uint32_t bufferedDurationMs(std::size_t byteCount, std::uint64_t bytesPerSecondValue) noexcept
    {
      if (bytesPerSecondValue == 0)
      {
        return 0;
      }

      return static_cast<std::uint32_t>((static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecondValue);
    }
  } // namespace

  MemorySource::MemorySource(std::unique_ptr<IDecoderSession> decoder, DecodedStreamInfo streamInfo)
    : _decoder{std::move(decoder)}, _streamInfo{streamInfo}
  {
  }

  ao::Result<> MemorySource::initialize()
  {
    auto const estimatedBytes =
      (static_cast<std::uint64_t>(_streamInfo.durationMs) * bytesPerSecond(_streamInfo.outputFormat)) / 1000U;

    if (estimatedBytes > 0 && estimatedBytes < static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
      _pcmBytes.reserve(static_cast<std::size_t>(estimatedBytes));
    }

    while (true)
    {
      auto const blockResult = _decoder->readNextBlock();

      if (!blockResult)
      {
        return std::unexpected(
          ao::Error{.code = ao::Error::Code::DecodeFailed, .message = blockResult.error().message});
      }

      auto const& block = *blockResult;
      if (block.endOfStream)
      {
        break;
      }

      _pcmBytes.insert(_pcmBytes.end(), block.bytes.begin(), block.bytes.end());
    }

    _decoder->close();
    return {};
  }

  std::size_t MemorySource::read(std::span<std::byte> output) noexcept
  {
    auto const offset = _readOffset.load(std::memory_order_acquire);
    auto const available = _pcmBytes.size() - offset;
    auto const toCopy = std::min(available, output.size());

    if (toCopy == 0)
    {
      return 0;
    }

    std::memcpy(output.data(), _pcmBytes.data() + offset, toCopy);
    _readOffset.store(offset + toCopy, std::memory_order_release);
    return toCopy;
  }

  bool MemorySource::isDrained() const noexcept
  {
    return _readOffset.load(std::memory_order_acquire) >= _pcmBytes.size();
  }

  std::uint32_t MemorySource::bufferedMs() const noexcept
  {
    auto const remaining = _pcmBytes.size() - _readOffset.load(std::memory_order_acquire);
    return bufferedDurationMs(remaining, bytesPerSecond(_streamInfo.outputFormat));
  }

  ao::Result<> MemorySource::seek(std::uint32_t positionMs)
  {
    _readOffset.store(positionToByteOffset(positionMs), std::memory_order_release);
    return {};
  }

  std::size_t MemorySource::positionToByteOffset(std::uint32_t positionMs) const noexcept
  {
    auto const frameByteCount = frameBytes(_streamInfo.outputFormat);

    if (frameByteCount == 0 || _streamInfo.outputFormat.sampleRate == 0)
    {
      return 0;
    }

    auto const frameIndex = (static_cast<std::uint64_t>(positionMs) * _streamInfo.outputFormat.sampleRate) / 1000U;
    auto const byteOffset = frameIndex * frameByteCount;
    auto const clampedOffset = std::min<std::uint64_t>(byteOffset, _pcmBytes.size());
    return static_cast<std::size_t>(clampedOffset - (clampedOffset % frameByteCount));
  }
} // namespace ao::audio
