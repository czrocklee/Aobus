// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"

#include <ao/audio/DecoderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ao::audio::test
{
  std::int16_t readSigned16LePcmSample(std::span<std::byte const> bytes, std::size_t index)
  {
    REQUIRE(index < bytes.size() / sizeof(std::int16_t));
    std::int16_t sample = 0;
    std::memcpy(&sample, bytes.subspan(index * sizeof(sample), sizeof(sample)).data(), sizeof(sample));
    return sample;
  }

  std::int32_t readSigned24PackedLePcmSample(std::span<std::byte const> bytes, std::size_t index)
  {
    constexpr std::uint32_t kSignBit = 0x00800000U;
    constexpr std::int32_t kRange = 0x01000000;
    REQUIRE(index < bytes.size() / 3U);
    auto const offset = index * 3U;
    auto const bits = static_cast<std::uint32_t>(bytes[offset]) |
                      (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
                      (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U);
    auto const sample = static_cast<std::int32_t>(bits);
    return (bits & kSignBit) != 0U ? sample - kRange : sample;
  }

  std::int32_t readSigned32LePcmSample(std::span<std::byte const> bytes, std::size_t index)
  {
    REQUIRE(index < bytes.size() / sizeof(std::int32_t));
    std::int32_t sample = 0;
    std::memcpy(&sample, bytes.subspan(index * sizeof(sample), sizeof(sample)).data(), sizeof(sample));
    return sample;
  }

  std::uint64_t readUntilStableEndOfStream(DecoderSession& decoder, std::size_t maxBlocks)
  {
    std::uint64_t totalFrames = 0;
    bool sawEndOfStream = false;

    for (std::size_t count = 0; count < maxBlocks && !sawEndOfStream; ++count)
    {
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);

      totalFrames += blockRes->frames;
      sawEndOfStream = blockRes->endOfStream;
    }

    REQUIRE(sawEndOfStream);

    auto const stableBlockRes = decoder.readNextBlock();
    REQUIRE(stableBlockRes);
    CHECK(stableBlockRes->endOfStream);
    CHECK(stableBlockRes->bytes.empty());
    return totalFrames;
  }

  TerminalReadResult readUntilTerminalState(DecoderSession& decoder, std::size_t maxBlocks)
  {
    auto result = TerminalReadResult{};

    for (std::size_t count = 0; count < maxBlocks; ++count)
    {
      auto const blockRes = decoder.readNextBlock();

      if (!blockRes)
      {
        result.optError = blockRes.error();
        return result;
      }

      result.frames += blockRes->frames;

      if (blockRes->endOfStream)
      {
        return result;
      }
    }

    FAIL("Decoder did not reach an error or end of stream");
  }
} // namespace ao::audio::test
