// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/audio/Format.h>
#include <ao/audio/Mp3DecoderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Mp3DecoderSession - malformed stream handling", "[audio][unit][mp3][error][malformed]")
  {
    auto const fixture = requireAudioFixture("basic_metadata.mp3");
    auto const source = readFileBytes(fixture);
    REQUIRE(source.size() > 1024);

    SECTION("Truncated audio reaches stable end of stream")
    {
      auto data = source;
      data.resize(data.size() / 2);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK_FALSE(terminal.optError);
      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);

      auto const stableBlock = decoder.readNextBlock();
      REQUIRE(stableBlock);
      CHECK(stableBlock->endOfStream);
      CHECK(stableBlock->bytes.empty());

      auto const info = decoder.streamInfo();
      REQUIRE(info.durationMs > 0);
      REQUIRE(decoder.seek(0));

      auto const restartedBlock = decoder.readNextBlock();
      REQUIRE(restartedBlock);
      CHECK(restartedBlock->frames > 0);
      CHECK_FALSE(restartedBlock->endOfStream);
    }

    SECTION("Corrupt frames are skipped without hanging")
    {
      auto data = source;
      auto const corruptionBegin = data.size() / 2;
      auto const corruptionEnd = corruptionBegin + 512;
      REQUIRE(corruptionEnd < data.size());
      std::fill(data.begin() + static_cast<std::ptrdiff_t>(corruptionBegin),
                data.begin() + static_cast<std::ptrdiff_t>(corruptionEnd),
                0xA5);

      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK_FALSE(terminal.optError);
      CHECK(terminal.frames > 0);

      auto const stableBlock = decoder.readNextBlock();
      REQUIRE(stableBlock);
      CHECK(stableBlock->endOfStream);
      CHECK(stableBlock->bytes.empty());
    }
  }
} // namespace ao::audio::test
