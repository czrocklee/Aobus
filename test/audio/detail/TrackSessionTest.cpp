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
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::detail::test
{
  namespace
  {
    // WAVE-style FL | FR, the canonical two-speaker identity at this boundary.
    constexpr std::uint32_t kFrontStereoMask = 0x3;
    // WAVE-style FL | FC, a different complete two-speaker identity.
    constexpr std::uint32_t kFrontLeftCenterMask = 0x5;

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

  TEST_CASE("TrackSession - preparation rejects a backend format without a whole PCM frame")
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

  TEST_CASE("TrackSession - preparation rejects a final source speaker mask that differs from inspection")
  {
    auto const evidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
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

  TEST_CASE("TrackSession - preparation rejects a final output speaker mask that differs from the backend mode")
  {
    auto const evidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
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

  TEST_CASE("TrackSession - preparation accepts evidence-only drift when effective formats stay stable")
  {
    auto const sourceFormat = stereoSource(kFrontStereoMask);
    auto const outputFormat = stereoOutput(kFrontStereoMask);
    auto inspectedEvidence = ChannelLayoutEvidence{};
    auto finalEvidence = ChannelLayoutEvidence{};

    SECTION("a reinterpreted origin keeps the same effective layout")
    {
      inspectedEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
      finalEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Inferred};
    }

    SECTION("identified history replaces absent history for the same layout")
    {
      inspectedEvidence = ChannelLayoutEvidence{};
      finalEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
    }

    auto const inspected = stereoStreamInfo(sourceFormat, outputFormat, inspectedEvidence);
    auto const finalInfo = stereoStreamInfo(sourceFormat, outputFormat, finalEvidence);

    auto const preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "evidence.wav"},
                                                   TrackSession::Inspection{.info = inspected},
                                                   outputFormat,
                                                   scriptedDecoderFactory(finalInfo));

    REQUIRE(preparedRes);
    CHECK(preparedRes->info == finalInfo);
  }

  TEST_CASE("TrackSession - an expensive initial offset is not decoded until activation", "[concurrency]")
  {
    auto const info = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), {});
    auto const offset = std::chrono::milliseconds{250};
    auto const targetPcm = std::vector{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto seekThread = std::thread::id{};
    auto const decoderFactory = [&](std::filesystem::path const&,
                                    std::optional<SampleEncoding>) -> Result<std::unique_ptr<DecoderSession>>
    {
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(info);
      decoderPtr->setReadScript({{std::vector(4, std::byte{0x11}), true}});
      decoderPtr->setSeekReadScript({{targetPcm, true}});
      decoderPtr->setWorkerSeek(offset,
                                [&](std::stop_token) -> Result<>
                                {
                                  seekThread = std::this_thread::get_id();
                                  return {};
                                });
      return decoderPtr;
    };

    auto preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "staged.wav"},
                                             TrackSession::Inspection{.info = info},
                                             info.outputFormat,
                                             decoderFactory,
                                             offset);
    REQUIRE(preparedRes);
    auto output = std::vector<std::byte>(targetPcm.size());
    CHECK(preparedRes->sourcePtr->read(output) == 0);
    CHECK_FALSE(preparedRes->sourcePtr->isDrained());
    CHECK(preparedRes->sourcePtr->isPreparingSeek());

    SECTION("discarding the candidate never executes the seek")
    {
      preparedRes->sourcePtr.reset();
      CHECK(seekThread == std::thread::id{});
    }

    SECTION("activation seeks off the caller thread before publishing the target PCM")
    {
      auto ready = std::counting_semaphore{0};
      auto readyToken = std::stop_token{};
      std::size_t readyBytes = 0;
      bool preparingAtReady = true;
      auto* const source = preparedRes->sourcePtr.get();
      auto opened = TrackSession::activate(std::move(*preparedRes),
                                           {},
                                           [&](std::stop_token token)
                                           {
                                             preparingAtReady = source->isPreparingSeek();
                                             readyBytes = source->read(output);
                                             readyToken = token;
                                             ready.release();
                                           });
      REQUIRE(ready.try_acquire_for(std::chrono::seconds{5}));
      CHECK_FALSE(readyToken.stop_requested());
      REQUIRE(opened.sourcePtr->seek(std::chrono::milliseconds{0}));
      CHECK(readyToken.stop_requested()); // The seek, not source destruction, retired the previous attempt.
      opened.sourcePtr.reset();
      CHECK_FALSE(ready.try_acquire());
      CHECK_FALSE(preparingAtReady);
      CHECK(readyBytes == output.size());
      CHECK(output == targetPcm);
      CHECK(seekThread != std::thread::id{});
      CHECK(seekThread != std::this_thread::get_id());
    }
  }

  TEST_CASE("TrackSession - inspection returns the scripted stream facts including channel-layout evidence")
  {
    auto const evidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
    auto const info = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);

    auto const inspectedRes =
      TrackSession::inspect(PlaybackInput{.filePath = "masked.wav"}, scriptedDecoderFactory(info));

    REQUIRE(inspectedRes);
    CHECK(inspectedRes->info == info);
  }

  TEST_CASE("TrackSession - prepared and activated track retain the inspected channel-layout evidence")
  {
    auto const evidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
    auto const info = stereoStreamInfo(stereoSource(kFrontStereoMask), stereoOutput(kFrontStereoMask), evidence);
    auto const backendFormat = stereoOutput(kFrontStereoMask);

    auto preparedRes = TrackSession::prepare(PlaybackInput{.filePath = "masked.wav"},
                                             TrackSession::Inspection{.info = info},
                                             backendFormat,
                                             scriptedDecoderFactory(info));

    REQUIRE(preparedRes);
    CHECK(preparedRes->info == info);
    CHECK(preparedRes->sourcePtr);

    auto const opened = TrackSession::activate(std::move(*preparedRes), {});

    CHECK(opened.info == info);
    CHECK(opened.sourcePtr);
  }
} // namespace ao::audio::detail::test
