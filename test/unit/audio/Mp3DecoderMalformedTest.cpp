// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/audio/Mp3DecoderSession.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Mp3DecoderSession - malformed stream handling", "[audio][unit][mp3][malformed]")
  {
    auto const fixture = requireAudioFixture("basic_metadata.mp3");
    auto const source = readFileBytes(fixture);
    REQUIRE(source.size() > 1024);

    SECTION("Truncated audio reaches stable end of stream")
    {
      auto data = source;
      data.resize(data.size() / 2);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK_FALSE(terminal.optError);
      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);

      auto const stableBlockRes = decoder.readNextBlock();
      REQUIRE(stableBlockRes);
      CHECK(stableBlockRes->endOfStream);
      CHECK(stableBlockRes->bytes.empty());

      auto const info = decoder.streamInfo();
      CHECK(info.duration > std::chrono::milliseconds{0});
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));

      auto const restartedBlockRes = decoder.readNextBlock();
      REQUIRE(restartedBlockRes);
      CHECK(restartedBlockRes->frames > 0);
      CHECK_FALSE(restartedBlockRes->endOfStream);
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
      auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK_FALSE(terminal.optError);
      CHECK(terminal.frames > 0);

      auto const stableBlockRes = decoder.readNextBlock();
      REQUIRE(stableBlockRes);
      CHECK(stableBlockRes->endOfStream);
      CHECK(stableBlockRes->bytes.empty());
    }
  }
} // namespace ao::audio::test
