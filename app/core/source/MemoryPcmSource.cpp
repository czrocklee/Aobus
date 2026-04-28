// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/source/MemoryPcmSource.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace app::core::source
{
  namespace
  {
    std::size_t frameBytes(AudioFormat const& format) noexcept
    {
      if (format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
      return static_cast<std::size_t>(format.channels) * bytesPerSample;
    }

    std::uint64_t bytesPerSecond(AudioFormat const& format) noexcept
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

  MemoryPcmSource::MemoryPcmSource(std::unique_ptr<decoder::IAudioDecoderSession> decoder,
                                   decoder::DecodedStreamInfo streamInfo)
    : _decoder{std::move(decoder)}, _streamInfo{streamInfo}
  {
  }

  bool MemoryPcmSource::initialize()
  {
    auto const estimatedBytes =
      (static_cast<std::uint64_t>(_streamInfo.durationMs) * bytesPerSecond(_streamInfo.outputFormat)) / 1000U;

    if (estimatedBytes > 0 && estimatedBytes < static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
      _pcmBytes.reserve(static_cast<std::size_t>(estimatedBytes));
    }

    while (true)
    {
      auto block = _decoder->readNextBlock();

      if (!block)
      {
        _lastError = std::string(_decoder->lastError());
        return false;
      }

      if (block->endOfStream)
      {
        break;
      }

      _pcmBytes.insert(_pcmBytes.end(), block->bytes.begin(), block->bytes.end());
    }

    _decoder->close();
    return true;
  }

  std::size_t MemoryPcmSource::read(std::span<std::byte> output) noexcept
  {
    auto lock = std::lock_guard<std::mutex>{_mutex};
    auto const available = _pcmBytes.size() - _readOffset;
    auto const toCopy = std::min(available, output.size());

    if (toCopy == 0)
    {
      return 0;
    }

    std::memcpy(output.data(), _pcmBytes.data() + _readOffset, toCopy);
    _readOffset += toCopy;
    return toCopy;
  }

  bool MemoryPcmSource::isDrained() const noexcept
  {
    auto lock = std::lock_guard<std::mutex>{_mutex};
    return _readOffset >= _pcmBytes.size();
  }

  std::uint32_t MemoryPcmSource::bufferedMs() const noexcept
  {
    auto lock = std::lock_guard<std::mutex>{_mutex};
    return bufferedDurationMs(_pcmBytes.size() - _readOffset, bytesPerSecond(_streamInfo.outputFormat));
  }

  bool MemoryPcmSource::seek(std::uint32_t positionMs)
  {
    auto lock = std::lock_guard<std::mutex>{_mutex};
    _readOffset = positionToByteOffset(positionMs);
    return true;
  }

  std::string MemoryPcmSource::lastError() const
  {
    return _lastError;
  }

  std::size_t MemoryPcmSource::positionToByteOffset(std::uint32_t positionMs) const noexcept
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

} // namespace app::core::source
