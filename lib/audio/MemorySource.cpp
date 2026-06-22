// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/MemorySource.h>
#include <ao/audio/Types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace ao::audio
{
  namespace
  {
    std::chrono::milliseconds calculateBufferedDuration(std::size_t byteCount,
                                                        std::uint64_t bytesPerSecondValue) noexcept
    {
      if (bytesPerSecondValue == 0)
      {
        return std::chrono::milliseconds{0};
      }

      return std::chrono::milliseconds{(static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecondValue};
    }
  } // namespace

  MemorySource::MemorySource(std::unique_ptr<IDecoderSession> decoderPtr, DecodedStreamInfo streamInfo)
    : _decoderPtr{std::move(decoderPtr)}
    , _streamInfo{streamInfo}
    , _bytesPerSecond{bytesPerSecond(streamInfo.outputFormat)}
  {
  }

  Result<> MemorySource::initialize()
  {
    auto const estimatedBytes = durationToSamples(_streamInfo.duration, _streamInfo.outputFormat.sampleRate) *
                                frameBytes(_streamInfo.outputFormat);

    if (estimatedBytes > 0 && estimatedBytes < static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
      _pcmBytes.reserve(static_cast<std::size_t>(estimatedBytes));
    }

    while (true)
    {
      auto const blockResult = _decoderPtr->readNextBlock();

      if (!blockResult)
      {
        return makeError(Error::Code::DecodeFailed, blockResult.error().message);
      }

      auto const& block = *blockResult;

      if (block.endOfStream)
      {
        break;
      }

      _pcmBytes.insert(_pcmBytes.end(), block.bytes.begin(), block.bytes.end());
    }

    _decoderPtr->close();
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

  std::chrono::milliseconds MemorySource::bufferedDuration() const noexcept
  {
    auto const remaining = _pcmBytes.size() - _readOffset.load(std::memory_order_acquire);
    return calculateBufferedDuration(remaining, _bytesPerSecond);
  }

  Result<> MemorySource::seek(std::chrono::milliseconds offset)
  {
    _readOffset.store(timeToByteOffset(offset), std::memory_order_release);
    return {};
  }

  std::size_t MemorySource::timeToByteOffset(std::chrono::milliseconds offset) const noexcept
  {
    auto const frameByteCount = frameBytes(_streamInfo.outputFormat);

    if (frameByteCount == 0 || _streamInfo.outputFormat.sampleRate == 0)
    {
      return 0;
    }

    auto const frameIndex = durationToSamples(offset, _streamInfo.outputFormat.sampleRate);
    auto const byteOffset = frameIndex * frameByteCount;
    auto const clampedOffset = std::min<std::uint64_t>(byteOffset, _pcmBytes.size());
    return static_cast<std::size_t>(clampedOffset - (clampedOffset % frameByteCount));
  }
} // namespace ao::audio
