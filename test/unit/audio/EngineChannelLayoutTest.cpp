// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    DecodedStreamInfo scriptedInfoWithLayout(PcmFormat const& format, ChannelLayoutEvidence evidence)
    {
      auto info = makeScriptedStreamInfo(format);
      info.sourceChannelLayout = evidence;
      return info;
    }
  } // namespace

  TEST_CASE("Engine - prewarm hint with a wrong speaker mask is ignored before the optimistic decoder open",
            "[audio][unit][engine][channel-layout]")
  {
    auto const sourceFormat =
      PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const hintEncoding = SampleEncoding::Signed24PackedLe;
    auto const markerData = std::vector{std::byte{0x51},
                                        std::byte{0x52},
                                        std::byte{0x53},
                                        std::byte{0x54},
                                        std::byte{0x55},
                                        std::byte{0x56},
                                        std::byte{0x57},
                                        std::byte{0x58}};

    // The same two stereo frames widened to packed24, authored independently:
    // 0x5251/0x5453 and 0x5655/0x5857 become left-aligned 24-bit samples.
    auto const hintedMarkerData = std::vector{std::byte{0x00},
                                              std::byte{0x51},
                                              std::byte{0x52},
                                              std::byte{0x00},
                                              std::byte{0x53},
                                              std::byte{0x54},
                                              std::byte{0x00},
                                              std::byte{0x55},
                                              std::byte{0x56},
                                              std::byte{0x00},
                                              std::byte{0x57},
                                              std::byte{0x58}};

    // The staged explicit-start path is synchronous on the calling thread, so a
    // plain request log is safe; no decoder is opened from an Engine callback.
    auto decoderRequests = std::vector<std::optional<SampleEncoding>>{};

    auto decoderFactory = [sourceFormat, &markerData, &hintedMarkerData, &decoderRequests](
                            std::filesystem::path const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.push_back(optOutputEncoding);
      auto const srcFormat = signalFormat(sourceFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = srcFormat,
        .outputFormat = pcmFormat(srcFormat, optOutputEncoding.value_or(sourceFormat.encoding)),
        .duration = std::chrono::milliseconds{10},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = optOutputEncoding == SampleEncoding::Signed24PackedLe ? hintedMarkerData : markerData,
          .endOfStream = false},
         {.endOfStream = true}});
      return decoderPtr;
    };

    SECTION("wrong mask is ignored before the optimistic decoder open")
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      // The fixed hint is an application-level prediction only; the fake's
      // open() stays the authoritative test-observed application format and
      // confirms no physical native endpoint.
      backendPtr->setPrewarmFormat(
        PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = hintEncoding, .speakerMask = 0x5});
      auto* const backendRaw = backendPtr.get();
      auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), decoderFactory};

      auto stagedRes = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "hinted.flac"}));

      REQUIRE(stagedRes);
      // The mask mismatch is resolved at hint selection: no optimistic decoder
      // was opened during staging, so this is not a failed preparation.
      REQUIRE(decoderRequests.size() == 1);
      CHECK_FALSE(decoderRequests.front());

      auto const committedRes = engine.commitPlayback(std::move(*stagedRes));

      REQUIRE(committedRes);
      CHECK(committedRes->playbackStarted);
      // The playing decoder is opened from the authoritative backend mode, and
      // the hint's encoding never reached any decoder request.
      REQUIRE(decoderRequests.size() == 2);
      CHECK(decoderRequests.back() == SampleEncoding::Signed16Le);
      auto const hintEncodingReached = [&](std::optional<SampleEncoding> const& optEncoding)
      { return optEncoding == hintEncoding; };
      CHECK(std::ranges::none_of(decoderRequests, hintEncodingReached));

      auto* const target = backendRaw->target();
      REQUIRE(target != nullptr);
      auto out = std::array<std::byte, 8>{};
      REQUIRE(target->renderPcm(out).bytesWritten == out.size());
      CHECK(std::vector<std::byte>{out.begin(), out.end()} == markerData);

      auto const status = engine.status();
      CHECK(status.transport == Transport::Playing);
      CHECK(status.routeState.sourceFormat.speakerMask == 0x3);
      CHECK(status.routeState.decoderOutputFormat.speakerMask == 0x3);
      CHECK(backendRaw->currentFormat().speakerMask == 0x3);
      CHECK(std::ranges::count(backendRaw->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    }

    SECTION("identical mask control accepts the hint")
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      backendPtr->setPrewarmFormat(
        PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = hintEncoding, .speakerMask = 0x3});
      auto* const backendRaw = backendPtr.get();
      auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), decoderFactory};

      auto stagedRes = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "hinted.flac"}));

      REQUIRE(stagedRes);
      // The matching mask lets the hint select the optimistic decoder encoding
      // during staging, before any backend open.
      REQUIRE(decoderRequests.size() == 2);
      CHECK_FALSE(decoderRequests.front());
      CHECK(decoderRequests.back() == hintEncoding);

      auto const committedRes = engine.commitPlayback(std::move(*stagedRes));

      REQUIRE(committedRes);
      CHECK(committedRes->playbackStarted);
      // open() stays authoritative: the fake selects Signed16Le, so the stale
      // prewarmed 24-bit decoder is discarded and reopened in the opened mode.
      REQUIRE(decoderRequests.size() == 3);
      CHECK(decoderRequests.back() == SampleEncoding::Signed16Le);

      auto* const target = backendRaw->target();
      REQUIRE(target != nullptr);
      auto out = std::array<std::byte, 8>{};
      REQUIRE(target->renderPcm(out).bytesWritten == out.size());
      CHECK(std::vector<std::byte>{out.begin(), out.end()} == markerData);

      auto const status = engine.status();
      CHECK(status.transport == Transport::Playing);
      CHECK(status.routeState.decoderOutputFormat.speakerMask == 0x3);
      CHECK(backendRaw->currentFormat().speakerMask == 0x3);
      CHECK(std::ranges::count(backendRaw->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    }
  }

  TEST_CASE("Engine - same-count successor with a different speaker mask drains instead of splicing",
            "[audio][unit][engine][channel-layout]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const firstFormat =
      PcmFormat{.sampleRate = 1000, .channels = 4, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x33};
    auto const secondFormat =
      PcmFormat{.sampleRate = 1000, .channels = 4, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x107};
    // Two complete frames per track; one 4-channel 16-bit frame is 8 bytes.
    CHECK(frameBytes(firstFormat) == 8);
    CHECK(frameBytes(secondFormat) == 8);
    auto const firstData = std::vector{std::byte{0x21},
                                       std::byte{0x22},
                                       std::byte{0x23},
                                       std::byte{0x24},
                                       std::byte{0x25},
                                       std::byte{0x26},
                                       std::byte{0x27},
                                       std::byte{0x28},
                                       std::byte{0x29},
                                       std::byte{0x2A},
                                       std::byte{0x2B},
                                       std::byte{0x2C},
                                       std::byte{0x2D},
                                       std::byte{0x2E},
                                       std::byte{0x2F},
                                       std::byte{0x30}};
    auto const secondData = std::vector{std::byte{0x41},
                                        std::byte{0x42},
                                        std::byte{0x43},
                                        std::byte{0x44},
                                        std::byte{0x45},
                                        std::byte{0x46},
                                        std::byte{0x47},
                                        std::byte{0x48},
                                        std::byte{0x49},
                                        std::byte{0x4A},
                                        std::byte{0x4B},
                                        std::byte{0x4C},
                                        std::byte{0x4D},
                                        std::byte{0x4E},
                                        std::byte{0x4F},
                                        std::byte{0x50}};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};

    auto engine =
      Engine{std::move(backendPtr),
             device,
             makePathScriptedDecoderFactory({
               {.path = "first-33.flac", .info = makeScriptedStreamInfo(firstFormat), .data = firstData},
               {.path = "second-107.flac", .info = makeScriptedStreamInfo(secondFormat), .data = secondData},
             })};

    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first-33.flac"}));
    auto const firstStatus = engine.status();
    REQUIRE(firstStatus.transport == Transport::Playing);
    CHECK(firstStatus.routeState.sourceFormat.speakerMask == 0x33);
    CHECK(firstStatus.routeState.decoderOutputFormat.speakerMask == 0x33);
    CHECK(backendRaw->currentFormat() == firstFormat);

    auto const preparedRes = engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second-107.flac"}));

    REQUIRE(preparedRes);
    CHECK(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 16>{};
    auto const firstRenderRes = target->renderPcm(firstOut);
    REQUIRE(firstRenderRes.bytesWritten == firstOut.size());
    CHECK(firstRenderRes.positionFrames == 2);
    CHECK(std::vector<std::byte>{firstOut.begin(), firstOut.end()} == firstData);

    auto tailOut = std::array<std::byte, 16>{};
    auto const drainedRenderRes = target->renderPcm(tailOut);
    CHECK(drainedRenderRes.drained);
    // The different-mask successor is never spliced: no successor PCM is
    // delivered by the draining render.
    CHECK(drainedRenderRes.bytesWritten == 0);
    CHECK(advancedLatch.count() == 0);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.tryWaitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);

    // The new mask only reaches the backend through an explicit subsequent
    // start, which reopens the route instead of splicing.
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "second-107.flac"}));
    auto const secondStatus = engine.status();
    REQUIRE(secondStatus.transport == Transport::Playing);
    CHECK(secondStatus.routeState.sourceFormat.speakerMask == 0x107);
    CHECK(secondStatus.routeState.decoderOutputFormat.speakerMask == 0x107);
    CHECK(secondStatus.routeState.engineOutputFormat.speakerMask == 0x107);
    CHECK(backendRaw->currentFormat() == secondFormat);

    auto const events = backendRaw->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 2);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 2);
    CHECK(advancedLatch.count() == 0);

    auto* const reopenedTarget = backendRaw->target();
    REQUIRE(reopenedTarget != nullptr);
    auto secondOut = std::array<std::byte, 16>{};
    REQUIRE(reopenedTarget->renderPcm(secondOut).bytesWritten == secondOut.size());
    CHECK(std::vector<std::byte>{secondOut.begin(), secondOut.end()} == secondData);
  }

  TEST_CASE("Engine - equal-mask successor splices and refreshes route source layout evidence",
            "[audio][unit][engine][channel-layout]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format =
      PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const firstEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
    auto const secondEvidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Inferred};
    auto const firstData = std::vector{std::byte{0x11},
                                       std::byte{0x12},
                                       std::byte{0x13},
                                       std::byte{0x14},
                                       std::byte{0x15},
                                       std::byte{0x16},
                                       std::byte{0x17},
                                       std::byte{0x18}};
    auto const secondData = std::vector{std::byte{0x21},
                                        std::byte{0x22},
                                        std::byte{0x23},
                                        std::byte{0x24},
                                        std::byte{0x25},
                                        std::byte{0x26},
                                        std::byte{0x27},
                                        std::byte{0x28}};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};

    auto engine = Engine{
      std::move(backendPtr),
      device,
      makePathScriptedDecoderFactory({
        {.path = "first-declared.flac", .info = scriptedInfoWithLayout(format, firstEvidence), .data = firstData},
        {.path = "second-inferred.flac", .info = scriptedInfoWithLayout(format, secondEvidence), .data = secondData},
      })};

    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first-declared.flac"}));
    REQUIRE(engine.status().transport == Transport::Playing);
    // Baseline: the explicit start publishes the first track's declaration.
    CHECK(engine.status().routeState.sourceChannelLayout == firstEvidence);

    auto const preparedRes = engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second-inferred.flac"}));

    REQUIRE(preparedRes);
    CHECK(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto out = std::array<std::byte, 8>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == firstData);
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);
    REQUIRE(advancedLatch.tryWaitForCount(1));
    CHECK(endedLatch.count() == 0);

    auto const status = engine.status();
    CHECK(status.transport == Transport::Playing);
    CHECK(status.routeState.sourceFormat.speakerMask == 0x3);
    CHECK(status.routeState.decoderOutputFormat.speakerMask == 0x3);
    CHECK(status.routeState.engineOutputFormat.speakerMask == 0x3);
    // Layout interpretation follows the successor, independently of native delivery.
    CHECK(status.routeState.sourceChannelLayout == secondEvidence);
    CHECK(status.routeState.sourceChannelLayout != firstEvidence);

    auto const route = engine.routeStatus();
    CHECK(route.state.sourceChannelLayout == secondEvidence);
    CHECK(route.state.engineOutputFormat.speakerMask == 0x3);

    auto const events = backendRaw->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
  }
} // namespace ao::audio::test
