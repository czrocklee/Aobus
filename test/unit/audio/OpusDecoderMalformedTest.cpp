// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/OpusDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("OpusDecoderSession - rejects streams it cannot open", "[audio][unit][opus][malformed]")
  {
    auto const openError = [](std::vector<std::uint8_t> const& bytes)
    {
      auto const temp = ao::test::TempFile{bytes, ".opus"};
      auto sessionRes = OpusDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(sessionRes);
      return sessionRes.error().code;
    };

    SECTION("Content that is not an Ogg stream is corrupt")
    {
      CHECK(openError(std::vector<std::uint8_t>(256, 0x41)) == Error::Code::CorruptData);
    }

    SECTION("A truncated first page is corrupt")
    {
      auto const source = readFileBytes(requireAudioFixture("basic_metadata.opus"));
      auto data = source;
      data.resize(16);
      CHECK(openError(data) == Error::Code::CorruptData);
    }

    SECTION("An identification packet with a foreign signature is corrupt")
    {
      auto data = readFileBytes(requireAudioFixture("basic_metadata.opus"));
      auto const magic = std::vector<std::uint8_t>{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
      auto const found = std::ranges::search(data, magic).begin();
      REQUIRE(found != data.end());
      *found = 'X';

      CHECK(openError(data) == Error::Code::CorruptData);
    }
  }

  TEST_CASE("OpusDecoderSession - a truncated stream reaches a stable end of stream", "[audio][unit][opus][malformed]")
  {
    auto const source = readFileBytes(requireAudioFixture("basic_metadata.opus"));
    REQUIRE(source.size() > 1024);

    // Cut into the last page so every page before it stays complete, which is
    // the shape an interrupted download leaves behind. An Ogg page is atomic, so
    // the truncated one is dropped and its granule position with it.
    auto const marker = std::vector<std::uint8_t>{'O', 'g', 'g', 'S'};
    auto data = source;
    auto const lastPage = std::ranges::find_end(data, marker).begin();
    REQUIRE(lastPage != data.begin());
    data.resize(static_cast<std::size_t>(std::distance(data.begin(), lastPage)) + 32);

    auto const temp = ao::test::TempFile{data, ".opus"};
    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const terminal = readUntilTerminalState(decoder, 512);
    CHECK_FALSE(terminal.optError);
    CHECK(terminal.frames > 0);
    CHECK(terminal.frames < 48000);

    auto const stableBlockRes = decoder.readNextBlock();
    REQUIRE(stableBlockRes);
    CHECK(stableBlockRes->endOfStream);
    CHECK(stableBlockRes->bytes.empty());

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));

    auto const restartedBlockRes = decoder.readNextBlock();
    REQUIRE(restartedBlockRes);
    CHECK(restartedBlockRes->frames > 0);
  }

  TEST_CASE("OpusDecoderSession - a stream truncated before any complete audio page is rejected",
            "[audio][unit][opus][malformed]")
  {
    auto const source = readFileBytes(requireAudioFixture("basic_metadata.opus"));
    auto data = source;

    // Half of this fixture lands inside its first audio page. Dropping that page
    // leaves the header packets alone, which is not a playable stream.
    data.resize(data.size() / 2);

    auto const temp = ao::test::TempFile{data, ".opus"};
    auto sessionRes = OpusDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(sessionRes);
    CHECK(sessionRes.error().code == Error::Code::FormatRejected);
  }
} // namespace ao::audio::test
