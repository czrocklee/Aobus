// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/TrackSession.h"

#include "../ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace ao::audio::detail::test
{
  TEST_CASE("TrackSession - preparation rejects a backend format without a whole PCM frame", "[audio][unit][engine]")
  {
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};
    auto framelessFormat = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};

    SECTION("zero channels")
    {
      framelessFormat.channels = 0;
    }

    SECTION("unknown sample encoding")
    {
      framelessFormat.encoding = SampleEncoding::Unknown;
    }

    auto const info = DecodedStreamInfo{
      .sourceFormat = sourceFormat, .outputFormat = framelessFormat, .duration = std::chrono::seconds{1}};
    auto const decoderFactory = [&info](std::filesystem::path const&,
                                        std::optional<SampleEncoding>) -> Result<std::unique_ptr<DecoderSession>>
    { return std::make_unique<audio::test::ScriptedDecoderSession>(info); };

    auto const preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "frameless.wav"},
                                                   TrackSession::Inspection{.info = info},
                                                   framelessFormat,
                                                   decoderFactory);

    REQUIRE_FALSE(preparedRes);
    CHECK(preparedRes.error().code == Error::Code::FormatRejected);
  }
} // namespace ao::audio::detail::test
