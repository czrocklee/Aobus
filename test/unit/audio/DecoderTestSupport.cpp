// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"

#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ao::audio::test
{
  void checkClosedSession(DecoderSession& decoder)
  {
    CHECK(!decoder.seek(std::chrono::milliseconds{1}));

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->endOfStream);
    CHECK(blockRes->bytes.empty());

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat == SignalFormat{});
    CHECK(info.outputFormat == PcmFormat{});
    CHECK(info.duration == std::chrono::milliseconds{0});
    CHECK_FALSE(info.isLossy);
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
