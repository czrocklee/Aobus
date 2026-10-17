// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "FlacFrameTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint32_t kStreamSampleRate = 48000;
    constexpr std::uint8_t kStreamChannels = 2;
    constexpr std::uint8_t kStreamBitsPerSample = 16;
    constexpr std::uint32_t kFrameSamples = 16;

    std::uint8_t computeCrc8(std::span<std::uint8_t const> data)
    {
      std::uint8_t value = 0;

      for (auto const byte : data)
      {
        value = static_cast<std::uint8_t>(value ^ byte);

        for (std::uint32_t bit = 0; bit < 8; ++bit)
        {
          auto const highBitSet = (value & 0x80U) != 0U;
          value = static_cast<std::uint8_t>((value << 1) ^ (highBitSet ? 0x07U : 0x00U));
        }
      }

      return value;
    }

    std::uint16_t computeCrc16(std::span<std::uint8_t const> data)
    {
      std::uint16_t value = 0;

      for (auto const byte : data)
      {
        value = static_cast<std::uint16_t>(value ^ (static_cast<std::uint16_t>(byte) << 8));

        for (std::uint32_t bit = 0; bit < 8; ++bit)
        {
          auto const highBitSet = (value & 0x8000U) != 0U;
          value = static_cast<std::uint16_t>((value << 1) ^ (highBitSet ? 0x8005U : 0x0000U));
        }
      }

      return value;
    }

    void appendBigEndianUnsigned(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::uint32_t width)
    {
      // A wider fill would shift a 64-bit value by its own width or more.
      REQUIRE(width <= 8U);

      for (auto byteIndex = width; byteIndex > 0; --byteIndex)
      {
        auto const shift = (byteIndex - 1U) * 8U;
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }

    void appendBigEndianSigned(std::vector<std::uint8_t>& bytes, std::int32_t value, std::uint32_t width)
    {
      appendBigEndianUnsigned(bytes, static_cast<std::uint64_t>(value), width);
    }

    std::uint8_t sampleRateCode(std::uint32_t sampleRate)
    {
      REQUIRE((sampleRate == kStreamSampleRate || sampleRate == 44100));

      return sampleRate == 44100 ? std::uint8_t{9} : std::uint8_t{10};
    }

    std::uint8_t bitsPerSampleCode(std::uint8_t bitsPerSample)
    {
      REQUIRE((bitsPerSample == 16 || bitsPerSample == 24));

      return bitsPerSample == 24 ? std::uint8_t{6} : std::uint8_t{4};
    }

    void appendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t const value)
    {
      for (std::uint32_t shift = 0; shift < 32; shift += 8)
      {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }
  } // namespace

  std::vector<std::uint8_t> buildFlacStreaminfoBytes(std::uint64_t totalSamples)
  {
    REQUIRE(totalSamples < (std::uint64_t{1} << 36U));

    // Stream marker, last-metadata-block flag with STREAMINFO type, length.
    auto bytes = std::vector<std::uint8_t>{'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22};

    appendBigEndianUnsigned(bytes, kFrameSamples, 2);
    appendBigEndianUnsigned(bytes, kFrameSamples, 2);
    appendBigEndianUnsigned(bytes, 0, 6);
    auto const packed = (static_cast<std::uint64_t>(kStreamSampleRate) << 44) |
                        (static_cast<std::uint64_t>(kStreamChannels - 1U) << 41) |
                        (static_cast<std::uint64_t>(kStreamBitsPerSample - 1U) << 36) | totalSamples;
    appendBigEndianUnsigned(bytes, packed, 8);
    // MD5 signature absent: sixteen zero bytes.
    bytes.insert(bytes.end(), std::size_t{16}, std::uint8_t{0});

    return bytes;
  }

  std::vector<std::uint8_t> buildFlacVerbatimFrameBytes(std::uint32_t frameIndex,
                                                        std::uint32_t sampleRate,
                                                        std::uint8_t channels,
                                                        std::uint8_t bitsPerSample,
                                                        std::span<std::int32_t const> markers)
  {
    REQUIRE((channels >= 1U && channels <= 8U));
    REQUIRE(markers.size() >= channels);
    // The authored frame number uses one UTF-8 byte rather than a general serializer.
    REQUIRE(frameIndex < 128U);

    // Frame sync, fixed blocksize, 16-sample block read from the header tail,
    // declared sample rate, channel assignment, and bit depth.
    auto bytes = std::vector<std::uint8_t>{};
    bytes.push_back(0xFF);
    bytes.push_back(0xF8);
    bytes.push_back(static_cast<std::uint8_t>((0x6U << 4) | sampleRateCode(sampleRate)));
    bytes.push_back(static_cast<std::uint8_t>(((static_cast<std::uint32_t>(channels) - 1U) << 4) |
                                              (static_cast<std::uint32_t>(bitsPerSampleCode(bitsPerSample)) << 1)));
    bytes.push_back(static_cast<std::uint8_t>(frameIndex));
    bytes.push_back(static_cast<std::uint8_t>(kFrameSamples - 1U));
    bytes.push_back(computeCrc8(bytes));

    for (std::uint8_t channel = 0; channel < channels; ++channel)
    {
      // Verbatim subframe header, no wasted bits.
      bytes.push_back(0x02);

      for (std::uint32_t sample = 0; sample < kFrameSamples; ++sample)
      {
        appendBigEndianSigned(bytes, markers[channel], bitsPerSample / 8U);
      }
    }

    appendBigEndianUnsigned(bytes, computeCrc16(bytes), 2);

    return bytes;
  }

  void appendFlacCommentBlock(std::vector<std::uint8_t>& bytes,
                              std::vector<std::string> const& comments,
                              bool const isLast)
  {
    auto body = std::vector<std::uint8_t>{};
    appendLe32(body, 0); // Empty vendor string.
    appendLe32(body, static_cast<std::uint32_t>(comments.size()));

    for (auto const& comment : comments)
    {
      appendLe32(body, static_cast<std::uint32_t>(comment.size()));
      body.insert(body.end(), comment.begin(), comment.end());
    }

    REQUIRE(body.size() <= 0xFFFFFFU);
    bytes.push_back(isLast ? 0x84 : 0x04);
    bytes.push_back(static_cast<std::uint8_t>((body.size() >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((body.size() >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(body.size() & 0xFFU));
    bytes.insert(bytes.end(), body.begin(), body.end());
  }
} // namespace ao::audio::test
