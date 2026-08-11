// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"

#include <ao/audio/DecoderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

namespace ao::audio::test
{
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
