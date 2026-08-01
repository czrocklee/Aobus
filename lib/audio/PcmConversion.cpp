// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/PcmConversion.h>

#include "detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint32_t kByteMask = 0xFFU;
    constexpr std::uint32_t kSigned24SignBit = 0x00800000U;
    constexpr std::uint32_t kSigned24ExtensionMask = 0xFF000000U;

    std::uint32_t readLittleEndian(std::span<std::byte const> bytes, std::size_t const offset, std::size_t const count)
    {
      std::uint32_t value = 0;

      for (std::size_t index = 0; index < count; ++index)
      {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + index])) << (index * 8U);
      }

      return value;
    }

    void writeLittleEndian(std::vector<std::byte>& bytes,
                           std::size_t const offset,
                           std::uint32_t const value,
                           std::size_t const count)
    {
      for (std::size_t index = 0; index < count; ++index)
      {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & kByteMask);
      }
    }

    std::int32_t signExtend24(std::uint32_t value) noexcept
    {
      if ((value & kSigned24SignBit) != 0U)
      {
        value |= kSigned24ExtensionMask;
      }

      return std::bit_cast<std::int32_t>(value);
    }

    std::int32_t readIntegerAsS32(std::span<std::byte const> source,
                                  std::size_t const offset,
                                  SampleEncoding const encoding)
    {
      switch (encoding)
      {
        case SampleEncoding::Signed16Le:
        {
          auto const raw = static_cast<std::uint16_t>(readLittleEndian(source, offset, 2U));
          auto const signedValue = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(raw));
          return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(signedValue) << 16U);
        }
        case SampleEncoding::Signed24PackedLe:
        case SampleEncoding::Signed24In32Le:
        {
          auto const signedValue =
            signExtend24(readLittleEndian(source, offset, encoding == SampleEncoding::Signed24PackedLe ? 3U : 4U));
          return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(signedValue) << 8U);
        }
        case SampleEncoding::Signed32Le: return std::bit_cast<std::int32_t>(readLittleEndian(source, offset, 4U));
        case SampleEncoding::Unknown:
        case SampleEncoding::Float32Le: return 0;
      }

      return 0;
    }
  } // namespace

  void unpackS24PcmSamples(std::span<std::byte const> source,
                           std::span<std::int32_t> destination,
                           std::uint8_t shift) noexcept
  {
    auto const count = std::min(source.size() / 3, destination.size());

    for (std::size_t i = 0; i < count; ++i)
    {
      auto const offset = i * 3;

      // Manual unpack for little-endian S24
      auto bits = static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset])) |
                  (static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset + 1])) << 8U) |
                  (static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset + 2])) << 16U);

      // Sign extension from 24 to 32 bits
      if ((bits & kSigned24SignBit) != 0U)
      {
        bits |= kSigned24ExtensionMask;
      }

      bits <<= shift;
      destination[i] = std::bit_cast<std::int32_t>(bits);
    }
  }

  Result<> convertPcmEncoding(std::span<std::byte const> source,
                              PcmFormat const& sourcePcmFormat,
                              SignalFormat const& sourceSignalFormat,
                              SampleEncoding const destinationEncoding,
                              std::vector<std::byte>& destination)
  {
    auto const sourceSampleBytes = bytesPerSample(sourcePcmFormat.encoding);
    auto const destinationSampleBytes = bytesPerSample(destinationEncoding);

    if (auto const sourceFrameBytes = frameBytes(sourcePcmFormat);
        sourceSampleBytes == 0U || destinationSampleBytes == 0U || sourceFrameBytes == 0U ||
        (source.size() % sourceFrameBytes) != 0U)
    {
      return makeError(Error::Code::InvalidInput, "Invalid PCM conversion format or byte count");
    }

    if (sourceSignalFormat.sampleRate != sourcePcmFormat.sampleRate ||
        sourceSignalFormat.channels != sourcePcmFormat.channels)
    {
      return makeError(Error::Code::InvalidInput, "PCM byte layout and logical signal format do not match");
    }

    if (!detail::isLosslessPcmEncoding(sourceSignalFormat, sourcePcmFormat.encoding) ||
        !detail::isLosslessPcmEncoding(sourceSignalFormat, destinationEncoding))
    {
      return makeError(Error::Code::NotSupported, "PCM conversion would lose source precision");
    }

    if (sourcePcmFormat.encoding == SampleEncoding::Float32Le && destinationEncoding != SampleEncoding::Float32Le)
    {
      return makeError(Error::Code::NotSupported, "Float PCM cannot be converted losslessly to integer PCM");
    }

    auto const sampleCount = source.size() / sourceSampleBytes;

    if (sampleCount > destination.max_size() / destinationSampleBytes)
    {
      return makeError(Error::Code::ValueTooLarge, "PCM conversion output size overflows the host address space");
    }

    destination.resize(sampleCount * destinationSampleBytes);

    if (sourcePcmFormat.encoding == destinationEncoding)
    {
      std::ranges::copy(source, destination.begin());
      return {};
    }

    if (destinationEncoding == SampleEncoding::Float32Le)
    {
      for (std::size_t sample = 0; sample < sampleCount; ++sample)
      {
        auto const integer = readIntegerAsS32(source, sample * sourceSampleBytes, sourcePcmFormat.encoding);
        auto const value = static_cast<float>(static_cast<double>(integer) / 2147483648.0);
        auto const bits = std::bit_cast<std::uint32_t>(value);
        writeLittleEndian(destination, sample * destinationSampleBytes, bits, destinationSampleBytes);
      }

      return {};
    }

    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
      auto const integer = readIntegerAsS32(source, sample * sourceSampleBytes, sourcePcmFormat.encoding);
      auto value = std::bit_cast<std::uint32_t>(integer);

      switch (destinationEncoding)
      {
        case SampleEncoding::Signed16Le: value >>= 16U; break;
        case SampleEncoding::Signed24PackedLe:
        case SampleEncoding::Signed24In32Le: value = static_cast<std::uint32_t>(integer >> 8U); break;
        case SampleEncoding::Signed32Le:
        case SampleEncoding::Unknown:
        case SampleEncoding::Float32Le: break;
      }

      writeLittleEndian(destination, sample * destinationSampleBytes, value, destinationSampleBytes);
    }

    return {};
  }
} // namespace ao::audio
