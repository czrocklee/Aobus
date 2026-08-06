// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/DecoderOutputAdapter.h"

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace ao::audio::detail::test
{
  TEST_CASE("DecoderOutputAdapter - configure rejects precision loss", "[audio][unit][decoder-output]")
  {
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};

    SECTION("native decoder encoding loses source precision")
    {
      auto adapter = DecoderOutputAdapter{std::nullopt};
      auto const configuredRes = adapter.configure(sourceFormat, SampleEncoding::Signed16Le);

      REQUIRE_FALSE(configuredRes);
      CHECK(configuredRes.error().code == Error::Code::NotSupported);
    }

    SECTION("requested output encoding loses source precision")
    {
      auto adapter = DecoderOutputAdapter{SampleEncoding::Signed16Le};
      auto const configuredRes = adapter.configure(sourceFormat, SampleEncoding::Signed24PackedLe);

      REQUIRE_FALSE(configuredRes);
      CHECK(configuredRes.error().code == Error::Code::NotSupported);
    }

    SECTION("floating-point native output cannot be converted back to integer PCM")
    {
      auto const integerSource = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};
      auto adapter = DecoderOutputAdapter{SampleEncoding::Signed16Le};
      auto const configuredRes = adapter.configure(integerSource, SampleEncoding::Float32Le);

      REQUIRE_FALSE(configuredRes);
      CHECK(configuredRes.error().code == Error::Code::NotSupported);
    }
  }

  TEST_CASE("DecoderOutputAdapter - matching encoding passes through native bytes", "[audio][unit][decoder-output]")
  {
    auto adapter = DecoderOutputAdapter{SampleEncoding::Signed16Le};
    auto const sourceFormat = SignalFormat{.sampleRate = 44100, .channels = 1, .precisionBits = 16};
    REQUIRE(adapter.configure(sourceFormat, SampleEncoding::Signed16Le));
    auto const bytes = std::array{std::byte{0x34}, std::byte{0x12}};

    auto const convertedRes = adapter.convert(bytes);

    REQUIRE(convertedRes);
    CHECK(convertedRes->data() == bytes.data());
    auto const expected = std::vector{std::byte{0x34}, std::byte{0x12}};
    CHECK(std::vector<std::byte>{convertedRes->begin(), convertedRes->end()} == expected);
  }

  TEST_CASE("DecoderOutputAdapter - requested encoding converts exact byte layout",
            "[audio][regression][decoder-output]")
  {
    auto adapter = DecoderOutputAdapter{SampleEncoding::Signed24In32Le};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 24};
    REQUIRE(adapter.configure(sourceFormat, SampleEncoding::Signed24PackedLe));
    auto const bytes = std::array{std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};

    auto const convertedRes = adapter.convert(bytes);

    REQUIRE(convertedRes);
    auto const expected = std::vector{std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x00}};
    CHECK(std::vector<std::byte>{convertedRes->begin(), convertedRes->end()} == expected);
  }

  TEST_CASE("DecoderOutputAdapter - widened native PCM may narrow to the requested lossless container",
            "[audio][regression][decoder-output]")
  {
    auto adapter = DecoderOutputAdapter{SampleEncoding::Signed24PackedLe};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16};
    REQUIRE(adapter.configure(sourceFormat, SampleEncoding::Signed32Le));
    auto const nativeBytes = std::array{std::byte{0x00}, std::byte{0x00}, std::byte{0x34}, std::byte{0x12}};

    auto const convertedRes = adapter.convert(nativeBytes);

    REQUIRE(convertedRes);
    auto const expected = std::vector{std::byte{0x00}, std::byte{0x34}, std::byte{0x12}};
    CHECK(std::vector<std::byte>{convertedRes->begin(), convertedRes->end()} == expected);
  }

  TEST_CASE("DecoderOutputAdapter - reset revokes the configured format", "[audio][unit][decoder-output]")
  {
    auto adapter = DecoderOutputAdapter{std::nullopt};
    auto const sourceFormat = SignalFormat{.sampleRate = 44100, .channels = 1, .precisionBits = 16};
    REQUIRE(adapter.configure(sourceFormat, SampleEncoding::Signed16Le));

    adapter.reset();

    CHECK(adapter.nativeFormat() == PcmFormat{});
    CHECK(adapter.outputFormat() == PcmFormat{});
  }
} // namespace ao::audio::detail::test
