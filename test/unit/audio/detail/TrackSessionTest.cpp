// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/TrackSession.h"

#include "../ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::audio::detail::test
{
  namespace
  {
    // WAVE-style FL | FR, the canonical two-speaker identity at this boundary.
    constexpr auto kFrontStereoMask = std::uint32_t{0x3};
    // WAVE-style FL | FC, a different complete two-speaker identity.
    constexpr auto kFrontLeftCenterMask = std::uint32_t{0x5};

    SignalFormat stereoSource(std::uint32_t const speakerMask)
    {
      return SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = speakerMask};
    }

    PcmFormat stereoOutput(std::uint32_t const speakerMask)
    {
      return PcmFormat{
        .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = speakerMask};
    }

    DecodedStreamInfo stereoStreamInfo(SignalFormat const& sourceFormat,
                                       PcmFormat const& outputFormat,
                                       ChannelLayoutEvidence const& evidence)
    {
      return DecodedStreamInfo{.sourceFormat = sourceFormat,
                               .outputFormat = outputFormat,
                               .duration = std::chrono::seconds{1},
                               .sourceChannelLayout = evidence};
    }

    // The factory reports the scripted final stream facts and one whole stereo
    // frame that ends the stream, so a successful prepare has preroll content
    // without a decode thread.
    TrackSession::DecoderFactoryFn scriptedDecoderFactory(DecodedStreamInfo const& info)
    {
      return
        [info](std::filesystem::path const&, std::optional<SampleEncoding>) -> Result<std::unique_ptr<DecoderSession>>
      {
        auto sessionPtr = std::make_unique<audio::test::ScriptedDecoderSession>(info);
        sessionPtr->setReadScript({audio::test::ScriptedDecoderSession::ReadScriptEntry{
          .data = std::vector{std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56}},
          .endOfStream = true}});
        return sessionPtr;
      };
    }
  } // namespace

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

  TEST_CASE("TrackSession - preparation rejects a final source speaker mask that differs from inspection",
            "[audio][unit][engine]")
  {
    auto const evidence =
      ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = kFrontStereoMask};
    auto const inspected = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);
    auto const drifted = stereoStreamInfo(stereoSource(kFrontLeftCenterMask), stereoOutput(kFrontStereoMask), evidence);

    auto const preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "masked.wav"},
                                                   TrackSession::Inspection{.info = inspected},
                                                   stereoOutput(kFrontStereoMask),
                                                   scriptedDecoderFactory(drifted));

    REQUIRE_FALSE(preparedRes);
    CHECK(preparedRes.error().code == Error::Code::FormatRejected);
    CHECK(preparedRes.error().message.contains("signal format changed after inspection"));
  }

  TEST_CASE("TrackSession - preparation rejects a final output speaker mask that differs from the backend mode",
            "[audio][unit][engine]")
  {
    auto const evidence =
      ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = kFrontStereoMask};
    auto const inspected = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);
    auto const unidentified = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(0U), evidence);

    auto const preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "masked.wav"},
                                                   TrackSession::Inspection{.info = inspected},
                                                   stereoOutput(kFrontStereoMask),
                                                   scriptedDecoderFactory(unidentified));

    REQUIRE_FALSE(preparedRes);
    CHECK(preparedRes.error().code == Error::Code::FormatRejected);
    CHECK(preparedRes.error().message.contains("PCM format selected by backend"));
  }

  TEST_CASE("TrackSession - preparation rejects drifted source channel-layout evidence despite equal formats",
            "[audio][unit][engine]")
  {
    auto const sourceFormat = stereoSource(kFrontStereoMask);
    auto const outputFormat = stereoOutput(kFrontStereoMask);
    auto inspectedEvidence = ChannelLayoutEvidence{};
    auto finalEvidence = ChannelLayoutEvidence{};

    SECTION("a different origin reinterprets the same declared mask")
    {
      inspectedEvidence =
        ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = kFrontStereoMask};
      finalEvidence =
        ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Inferred, .optDeclaredMask = kFrontStereoMask};
    }

    SECTION("an explicit zero declaration replaces an absent declaration")
    {
      inspectedEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared};
      finalEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = 0U};
    }

    auto const inspected = stereoStreamInfo(sourceFormat, outputFormat, inspectedEvidence);
    auto const finalInfo = stereoStreamInfo(sourceFormat, outputFormat, finalEvidence);

    auto const preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "evidence.wav"},
                                                   TrackSession::Inspection{.info = inspected},
                                                   outputFormat,
                                                   scriptedDecoderFactory(finalInfo));

    REQUIRE_FALSE(preparedRes);
    CHECK(preparedRes.error().code == Error::Code::FormatRejected);
    CHECK(preparedRes.error().message.contains("channel-layout evidence changed after inspection"));
  }

  TEST_CASE("TrackSession - inspection returns the scripted stream facts including channel-layout evidence",
            "[audio][unit][engine]")
  {
    auto const evidence =
      ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = kFrontStereoMask};
    auto const info = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);

    auto const inspectedRes =
      TrackSession::inspect(PlaybackInput{.filePath = "masked.wav"}, scriptedDecoderFactory(info));

    REQUIRE(inspectedRes);
    CHECK(inspectedRes->info == info);
  }

  TEST_CASE("TrackSession - prepared and activated track retain the inspected channel-layout evidence",
            "[audio][unit][engine]")
  {
    auto const evidence =
      ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = kFrontStereoMask};
    auto const info = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);
    auto const backendFormat = stereoOutput(kFrontStereoMask);

    auto preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "masked.wav"},
                                             TrackSession::Inspection{.info = info},
                                             backendFormat,
                                             scriptedDecoderFactory(info));

    REQUIRE(preparedRes);
    CHECK(preparedRes->info == info);
    CHECK(preparedRes->backendFormat == backendFormat);
    CHECK(preparedRes->sourcePtr);

    auto const opened = TrackSession::activate(std::move(*preparedRes), {});

    CHECK(opened.info == info);
    CHECK(opened.backendFormat == backendFormat);
    CHECK(opened.sourcePtr);
  }
} // namespace ao::audio::detail::test
