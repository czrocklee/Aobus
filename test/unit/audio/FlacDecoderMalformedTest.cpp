// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("FlacDecoderSession - malformed stream handling", "[audio][unit][flac][malformed]")
  {
    auto const fixture = requireAudioFixture("basic_metadata.flac");
    auto const source = readFileBytes(fixture);
    CHECK(source.size() > 1024);

    SECTION("Truncated STREAMINFO fails during open")
    {
      auto data = source;
      data.resize(16);
      auto const temp = ao::test::TempFile{data, ".flac"};
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      auto const result = decoder.open(temp.path);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::DecodeFailed);
      checkClosedSession(decoder);
    }

    SECTION("Truncated audio reaches a stable terminal state")
    {
      auto data = source;
      data.resize(data.size() / 2);
      auto const temp = ao::test::TempFile{data, ".flac"};
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);
      REQUIRE(terminal.optError);
      CHECK(terminal.optError->code == Error::Code::DecodeFailed);
    }

    SECTION("Corrupt audio frame does not hang or overrun the stream")
    {
      auto data = source;
      auto const corruptionBegin = data.size() / 2;
      auto const corruptionEnd = corruptionBegin + 256;
      REQUIRE(corruptionEnd < data.size());
      std::fill(data.begin() + static_cast<std::ptrdiff_t>(corruptionBegin),
                data.begin() + static_cast<std::ptrdiff_t>(corruptionEnd),
                0xA5);

      auto const temp = ao::test::TempFile{data, ".flac"};
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);
      REQUIRE(terminal.optError);
      CHECK(terminal.optError->code == Error::Code::DecodeFailed);
    }
  }
} // namespace ao::audio::test
