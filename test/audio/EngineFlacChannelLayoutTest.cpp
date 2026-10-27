// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineFailureTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    std::vector<std::uint8_t> repeatLiteralFrame(std::span<std::uint8_t const> frame, std::uint32_t frameCount = 48)
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
      {
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    std::vector<std::uint8_t> makeLayoutFlacBytes(std::uint8_t channels,
                                                  std::span<std::int32_t const> markers,
                                                  std::span<std::string const> comments = {})
    {
      auto bytes = buildFlacStreaminfoBytes(48, channels, 16);

      if (!comments.empty())
      {
        appendFlacCommentBlock(bytes, comments);
      }

      for (std::uint32_t frameIndex = 0; frameIndex < 3; ++frameIndex)
      {
        auto const frame = buildFlacVerbatimFrameBytes(frameIndex, 48000, channels, 16, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    std::vector<std::uint8_t> makeTruncatedMetadataFlacBytes()
    {
      auto bytes = buildFlacStreaminfoBytes(48, 2, 16);
      auto const commentHeaderOffset = bytes.size();
      auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"});
      appendFlacCommentBlock(bytes, comments);
      // The comment payload is complete, but its outer block now extends beyond
      // the file. Native metadata failure must not become optional Unknown.
      bytes[commentHeaderOffset + 1U] = 0xFF;
      bytes[commentHeaderOffset + 2U] = 0xFF;
      bytes[commentHeaderOffset + 3U] = 0xFF;
      return bytes;
    }

    void checkRouteIdentity(AudioRouteFormatState const& route,
                            std::uint8_t channels,
                            std::uint32_t mask,
                            ChannelLayoutOrigin origin,
                            AudioCodec codec)
    {
      CHECK(route.codec == codec);
      CHECK_FALSE(route.isLossySource);
      CHECK(route.sourceFormat.sampleRate == 48000);
      CHECK(route.sourceFormat.channels == channels);
      CHECK(route.sourceFormat.precisionBits == 16);
      CHECK(route.sourceFormat.speakerMask == mask);
      CHECK(route.decoderOutputFormat.sampleRate == 48000);
      CHECK(route.decoderOutputFormat.channels == channels);
      CHECK(route.decoderOutputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(route.decoderOutputFormat.speakerMask == mask);
      CHECK(route.engineOutputFormat == route.decoderOutputFormat);
      CHECK(route.sourceChannelLayout == ChannelLayoutEvidence{.origin = origin});
    }
  } // namespace

  TEST_CASE("Engine - matching real WAV and FLAC identities reuse a strict backend for one through eight channels",
            "[concurrency]")
  {
    auto const channels = GENERATE(std::uint8_t{1},
                                   std::uint8_t{2},
                                   std::uint8_t{3},
                                   std::uint8_t{4},
                                   std::uint8_t{5},
                                   std::uint8_t{6},
                                   std::uint8_t{7},
                                   std::uint8_t{8});
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(channels, wavFirst);
    // WAV defaults in Aobus's canonical speaker representation, not a decoder table:
    // 1: FC; 2: FL FR; 3: FL FR FC; 4: FL FR BL BR; 5: FL FR FC BL BR;
    // 6: FL FR FC LFE BL BR; 7: FL FR FC LFE BC SL SR; 8: FL FR FC LFE BL BR SL SR.
    // FLAC needs explicit back-role declarations at five/six channels to match.
    auto const masks = std::to_array<std::uint32_t>({0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F});
    auto const expectedMask = masks[channels - 1U];
    auto const wavOrigin = channels <= 2 ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Inferred;
    // Each successive marker labels the corresponding slot listed above. The encoded
    // FLAC input and the literal LE output oracle are authored separately.
    auto const encodedMarkers =
      std::to_array<std::int32_t>({0x1211, 0x2423, 0x3635, 0x4847, 0x5A59, 0x6C6B, 0x7E7D, 0x102F});
    auto const flacFrame = std::to_array<std::uint8_t>(
      {0x11, 0x12, 0x23, 0x24, 0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C, 0x7D, 0x7E, 0x2F, 0x10});
    auto const wavFrame = std::to_array<std::uint8_t>(
      {0x13, 0x14, 0x25, 0x26, 0x37, 0x38, 0x49, 0x4A, 0x5B, 0x5C, 0x6D, 0x6E, 0x7F, 0x70, 0x31, 0x32});
    auto const bytesPerFrame = static_cast<std::size_t>(channels) * 2U;
    auto const wavPcm = repeatLiteralFrame(std::span<std::uint8_t const>{wavFrame}.first(bytesPerFrame));
    auto const flacPcm = repeatLiteralFrame(std::span<std::uint8_t const>{flacFrame}.first(bytesPerFrame));
    auto const wavFile = ao::test::TempFile{
      ao::test::wav::makeWav({.channels = channels, .sampleRate = 48000, .audioData = wavPcm}), ".wav"};
    auto comments = std::vector<std::string>{};

    if (channels == 5U)
    {
      comments.emplace_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x37");
    }
    else if (channels == 6U)
    {
      comments.emplace_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F");
    }

    auto const flacFile = ao::test::TempFile{
      makeLayoutFlacBytes(channels, std::span<std::int32_t const>{encodedMarkers}.first(channels), comments), ".flac"};
    auto const& firstPath = wavFirst ? wavFile.path : flacFile.path;
    auto const& nextPath = wavFirst ? flacFile.path : wavFile.path;
    auto const& firstPcm = wavFirst ? wavPcm : flacPcm;
    auto const& nextPcm = wavFirst ? flacPcm : wavPcm;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto advancedEvents = std::vector<Engine::TrackAdvanced>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced(
      [&](Engine::TrackAdvanced const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          advancedEvents.push_back(event);
        }

        advanced.notify();
      });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    auto const firstItem = makePlaybackItem(firstPath);
    auto const nextItem = makePlaybackItem(nextPath);
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    // Source identity is already present before the first render.
    checkRouteIdentity(engine.status().routeState,
                       channels,
                       expectedMask,
                       wavFirst ? wavOrigin : ChannelLayoutOrigin::Identified,
                       wavFirst ? AudioCodec::Wav : AudioCodec::Flac);
    auto const installedFormat = backend->currentFormat();
    CHECK(installedFormat.speakerMask == expectedMask);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);

    auto expected = firstPcm;
    expected.insert(expected.end(), nextPcm.begin(), nextPcm.end());
    auto captured = std::vector<std::byte>{};
    auto output = std::vector<std::byte>(20U * bytesPerFrame);
    auto const firstRender = target->renderPcm(output);
    REQUIRE(firstRender.bytesWritten == output.size());
    CHECK_FALSE(firstRender.drained);
    CHECK(
      std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm}).first(output.size())));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(firstRender.positionFrames);

    // 28 old frames + 4 new frames in this one callback prove an actual splice.
    output.resize(32U * bytesPerFrame);
    auto const mixedRender = target->renderPcm(output);
    REQUIRE(mixedRender.bytesWritten == output.size());
    CHECK_FALSE(mixedRender.drained);
    CHECK(mixedRender.positionFrameOffset == 28);
    CHECK(mixedRender.positionFrames == 4);
    CHECK(
      std::ranges::equal(std::span{output}.first(28U * bytesPerFrame),
                         utility::bytes::view(std::span<std::uint8_t const>{firstPcm}).subspan(20U * bytesPerFrame)));
    CHECK(std::ranges::equal(std::span{output}.subspan(28U * bytesPerFrame),
                             utility::bytes::view(std::span<std::uint8_t const>{nextPcm}).first(4U * bytesPerFrame)));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(mixedRender.positionFrames);
    REQUIRE(advanced.tryWaitForCount(1));
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK(engine.transport() == Transport::Playing);
    checkRouteIdentity(engine.status().routeState,
                       channels,
                       expectedMask,
                       wavFirst ? ChannelLayoutOrigin::Identified : wavOrigin,
                       wavFirst ? AudioCodec::Flac : AudioCodec::Wav);
    CHECK(engine.routeStatus().generation == preparedRes->generation);
    CHECK(backend->target() == target);
    CHECK(backend->currentFormat() == installedFormat);

    output.resize(44U * bytesPerFrame);
    auto const finalRender = target->renderPcm(output);
    REQUIRE(finalRender.bytesWritten == output.size());
    CHECK_FALSE(finalRender.drained);
    CHECK(std::ranges::equal(
      output, utility::bytes::view(std::span<std::uint8_t const>{nextPcm}).subspan(4U * bytesPerFrame)));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(finalRender.positionFrames);
    CHECK(std::ranges::equal(captured, utility::bytes::view(std::span<std::uint8_t const>{expected})));
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(advanced.count() == 1);
    CHECK(ended.count() == 0);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(events, std::string{"close"}, &FakeCapturingBackend::Event::name) == 0);
    auto const lock = std::scoped_lock{callbackMutex};
    REQUIRE(advancedEvents.size() == 1);
    CHECK(advancedEvents.front().itemId == nextItem.id);
    CHECK(advancedEvents.front().input.filePath == nextPath);
    CHECK(advancedEvents.front().generation == preparedRes->generation);
  }

  TEST_CASE("Engine - unequal real WAV and FLAC identities drain before explicit successor reopen", "[concurrency]")
  {
    auto const channels = GENERATE(std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6});
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(channels, wavFirst);
    // Quad has explicit different roles (FL FR FC BC); untagged five/six
    // channels have unresolved back/side geometry and cannot match known WAV.
    auto const wavMask = std::to_array<std::uint32_t>({0x33, 0x37, 0x3F})[channels - 4U];
    std::uint32_t const flacMask = channels == 4U ? 0x107U : 0U;
    auto const flacOrigin = channels == 4U ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown;
    auto const wavFrame =
      std::to_array<std::uint8_t>({0x11, 0x12, 0x23, 0x24, 0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C});
    auto const flacFrame =
      std::to_array<std::uint8_t>({0x51, 0x52, 0x63, 0x64, 0x75, 0x76, 0x07, 0x08, 0x19, 0x1A, 0x2B, 0x2C});
    auto const bytesPerFrame = static_cast<std::size_t>(channels) * 2U;
    auto const wavPcm = repeatLiteralFrame(std::span<std::uint8_t const>{wavFrame}.first(bytesPerFrame));
    auto const flacPcm = repeatLiteralFrame(std::span<std::uint8_t const>{flacFrame}.first(bytesPerFrame));
    auto const markers = std::to_array<std::int32_t>({0x5251, 0x6463, 0x7675, 0x0807, 0x1A19, 0x2C2B});
    auto comments = std::vector<std::string>{};

    if (channels == 4U)
    {
      comments.emplace_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x107");
    }

    auto const wavFile = ao::test::TempFile{
      ao::test::wav::makeWav({.channels = channels, .sampleRate = 48000, .audioData = wavPcm}), ".wav"};
    auto const flacFile = ao::test::TempFile{makeLayoutFlacBytes(channels, markers, comments), ".flac"};
    auto const& firstPath = wavFirst ? wavFile.path : flacFile.path;
    auto const& nextPath = wavFirst ? flacFile.path : wavFile.path;
    auto const& firstPcm = wavFirst ? wavPcm : flacPcm;
    auto const& nextPcm = wavFirst ? flacPcm : wavPcm;
    auto const firstMask = wavFirst ? wavMask : flacMask;
    auto const nextMask = wavFirst ? flacMask : wavMask;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto endedEvents = std::vector<Engine::TrackEnded>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          endedEvents.push_back(event);
        }

        ended.notify();
      });
    auto const firstItem = makePlaybackItem(firstPath);
    auto const nextItem = makePlaybackItem(nextPath);
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const firstGeneration = engine.playbackGeneration();
    auto const firstRoute = engine.routeStatus();
    checkRouteIdentity(firstRoute.state,
                       channels,
                       firstMask,
                       wavFirst ? ChannelLayoutOrigin::Inferred : flacOrigin,
                       wavFirst ? AudioCodec::Wav : AudioCodec::Flac);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    CHECK_FALSE(engine.clearNext());
    auto output = std::vector<std::byte>(firstPcm.size());
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == output.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    target->handlePositionAdvanced(rendered.positionFrames);
    auto const drain = target->renderPcm(output);
    REQUIRE(drain.drained);
    CHECK(drain.bytesWritten == 0);
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 0);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK(engine.routeStatus() == firstRoute);
    CHECK(backend->target() == target);
    CHECK(backend->currentFormat().speakerMask == firstMask);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(engine.transport() == Transport::Idle);

    engine.play(nextItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    checkRouteIdentity(engine.status().routeState,
                       channels,
                       nextMask,
                       wavFirst ? flacOrigin : ChannelLayoutOrigin::Inferred,
                       wavFirst ? AudioCodec::Flac : AudioCodec::Wav);
    CHECK(backend->currentFormat().speakerMask == nextMask);
    auto* const reopenedTarget = backend->target();
    REQUIRE(reopenedTarget != nullptr);
    auto const reopenedRender = reopenedTarget->renderPcm(output);
    REQUIRE(reopenedRender.bytesWritten == output.size());
    CHECK_FALSE(reopenedRender.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{nextPcm})));
    reopenedTarget->handlePositionAdvanced(reopenedRender.positionFrames);
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(2));
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 1);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 2);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 2);
    CHECK(std::ranges::count(events, std::string{"close"}, &FakeCapturingBackend::Event::name) == 1);
    auto const lock = std::scoped_lock{callbackMutex};
    REQUIRE(endedEvents.size() == 1);
    CHECK(endedEvents.front().generation == firstGeneration);
  }

  TEST_CASE("Engine - truncated FLAC metadata is a recoverable source refusal before backend open")
  {
    auto const file = ao::test::TempFile{makeTruncatedMetadataFlacBytes(), ".flac"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    auto failureFuture = captureNextFailure(engine);
    auto const item = makePlaybackItem(file.path);
    engine.play(item);
    auto const failure = requireFailure(failureFuture);
    CHECK(failure.itemId == item.id);
    CHECK(failure.input.filePath == file.path);
    CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
    CHECK(failure.recoverable);
    CHECK(failure.error.code == Error::Code::DecodeFailed);
    CHECK(failure.error.message.contains("FLAC"));
    CHECK(failure.error.message.contains("metadata"));
    CHECK(engine.transport() == Transport::Idle);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(events, std::string{"open-failed"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 0);
  }

  TEST_CASE("Engine - truncated FLAC replacement clears armed lookahead without interrupting current playback",
            "[concurrency]")
  {
    auto const currentPcm = repeatLiteralFrame(std::to_array<std::uint8_t>({0x11, 0x12, 0x23, 0x24}));
    auto const currentFile =
      ao::test::TempFile{ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = currentPcm}), ".wav"};
    auto const markers = std::to_array<std::int32_t>({0x5251, 0x6463});
    auto const successorFile = ao::test::TempFile{makeLayoutFlacBytes(2, markers), ".flac"};
    auto const refusedFile = ao::test::TempFile{makeTruncatedMetadataFlacBytes(), ".flac"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto failures = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto endedEvents = std::vector<Engine::TrackEnded>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failures.notify(); });
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          endedEvents.push_back(event);
        }

        ended.notify();
      });
    auto const currentItem = makePlaybackItem(currentFile.path);
    auto const successorItem = makePlaybackItem(successorFile.path);
    auto const refusedItem = makePlaybackItem(refusedFile.path);
    engine.play(currentItem);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const preparedRes = engine.setNext(successorItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == successorItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto const currentRoute = engine.routeStatus();
    auto const currentGeneration = engine.playbackGeneration();
    auto const currentFormat = backend->currentFormat();
    auto const currentEvents = backend->events();
    auto* const currentTarget = backend->target();
    REQUIRE(currentTarget != nullptr);

    auto const refusedRes = engine.setNext(refusedItem);
    REQUIRE_FALSE(refusedRes);
    CHECK(refusedRes.error().code == Error::Code::DecodeFailed);
    CHECK(refusedRes.error().message.contains("FLAC"));
    CHECK(refusedRes.error().message.contains("metadata"));
    // The worker-prepared successor must be retired, not adopted by the audio
    // side later. Settle the control queue before asserting callback absence.
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(failures.count() == 0);
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 0);
    CHECK(engine.isCurrentPlaybackItem(currentItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(successorItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(refusedItem.id));
    CHECK(engine.playbackGeneration() == currentGeneration);
    CHECK(engine.routeStatus() == currentRoute);
    CHECK(engine.transport() == Transport::Playing);
    REQUIRE(backend->target() == currentTarget);
    CHECK(backend->currentFormat() == currentFormat);
    CHECK(std::ranges::equal(backend->events(),
                             currentEvents,
                             [](FakeCapturingBackend::Event const& lhs, FakeCapturingBackend::Event const& rhs)
                             { return lhs.name == rhs.name && lhs.format == rhs.format; }));
    REQUIRE_FALSE(engine.clearNext());

    auto output = std::vector<std::byte>(currentPcm.size());
    auto const rendered = currentTarget->renderPcm(output);
    REQUIRE(rendered.bytesWritten == output.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{currentPcm})));
    currentTarget->handlePositionAdvanced(rendered.positionFrames);
    auto const drain = currentTarget->renderPcm(output);
    REQUIRE(drain.drained);
    REQUIRE(drain.bytesWritten == 0);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(2));
    CHECK(failures.count() == 0);
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 1);
    CHECK(engine.transport() == Transport::Idle);
    auto const lock = std::scoped_lock{callbackMutex};
    REQUIRE(endedEvents.size() == 1);
    CHECK(endedEvents.front().generation == currentGeneration);
  }
} // namespace ao::audio::test
