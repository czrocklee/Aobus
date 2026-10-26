// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineFailureTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

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
  TEST_CASE("Engine - real WAV layouts control successor reuse without rewriting canonical samples",
            "[audio][integration][engine][wav][concurrency]")
  {
    // First track: plain quad is inferred FL FR BL BR. These complete S16LE
    // frames and the successor frames are independently authored slot markers.
    auto const firstPcm = std::vector<std::uint8_t>{
      0x11,
      0x12,
      0x21,
      0x22,
      0x31,
      0x32,
      0x41,
      0x42,
      0x51,
      0x52,
      0x61,
      0x62,
      0x71,
      0x72,
      0x81,
      0x82,
    };
    auto const secondPcm = std::vector<std::uint8_t>{
      0x91,
      0x92,
      0xA1,
      0xA2,
      0xB1,
      0xB2,
      0xC1,
      0xC2,
      0xD1,
      0xD2,
      0xE1,
      0xE2,
      0xF1,
      0xF2,
      0x01,
      0x02,
    };
    auto const firstBytes = ao::test::wav::makeWav({.channels = 4, .audioData = firstPcm});
    auto const firstFile = ao::test::TempFile{firstBytes, ".wav"};
    std::uint32_t nextMask = 0x33;

    SECTION("equal effective identity splices and refreshes inferred to declared history")
    {
    }

    SECTION("same count but FL FR FC BC identity drains and reopens")
    {
      nextMask = 0x107;
    }

    auto const secondBytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                     .channels = 4,
                                                     .channelMask = nextMask,
                                                     .audioData = secondPcm});
    auto const secondFile = ao::test::TempFile{secondBytes, ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto advancedEvents = std::vector<Engine::TrackAdvanced>{};
    auto endedEvents = std::vector<Engine::TrackEnded>{};
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
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          endedEvents.push_back(event);
        }

        ended.notify();
      });

    auto const firstItem = makePlaybackItem(firstFile.path);
    auto const secondItem = makePlaybackItem(secondFile.path);
    engine.play(firstItem);
    auto const firstGeneration = engine.routeStatus().generation;
    auto const firstStatus = engine.status();
    REQUIRE(firstStatus.transport == Transport::Playing);
    CHECK(firstStatus.routeState.sourceFormat.speakerMask == 0x33);
    CHECK(firstStatus.routeState.decoderOutputFormat.speakerMask == 0x33);
    CHECK(firstStatus.routeState.engineOutputFormat.speakerMask == 0x33);
    CHECK(firstStatus.routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::Inferred);
    CHECK(backend->currentFormat().speakerMask == 0x33);

    auto const preparedRes = engine.setNext(secondItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition ==
          (nextMask == 0x33 ? Engine::PreparedTransitionMode::Gapless : Engine::PreparedTransitionMode::DrainFallback));
    auto* const firstTarget = backend->target();
    REQUIRE(firstTarget != nullptr);
    auto output = std::array<std::byte, 16>{};
    auto const firstRender = firstTarget->renderPcm(output);
    REQUIRE(firstRender.bytesWritten == output.size());
    CHECK_FALSE(firstRender.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    firstTarget->handlePositionAdvanced(firstRender.positionFrames);

    if (nextMask == 0x33)
    {
      auto const secondRender = firstTarget->renderPcm(output);
      REQUIRE(secondRender.bytesWritten == output.size());
      CHECK_FALSE(secondRender.drained);
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
      firstTarget->handlePositionAdvanced(secondRender.positionFrames);
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(advanced.count() == 1);
      CHECK(ended.count() == 0);
      CHECK(engine.status().routeState.sourceChannelLayout ==
            ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified});
    }
    else
    {
      auto const drain = firstTarget->renderPcm(output);
      REQUIRE(drain.drained);
      REQUIRE(drain.bytesWritten == 0);
      CHECK(advanced.count() == 0);
      backend->emitDrainComplete();
      REQUIRE(ended.tryWaitForCount(1));
      CHECK(ended.count() == 1);
      CHECK(advanced.count() == 0);
      REQUIRE(engine.status().transport == Transport::Idle);
      engine.play(secondItem);
      REQUIRE(engine.status().transport == Transport::Playing);
      auto* const reopenedTarget = backend->target();
      REQUIRE(reopenedTarget != nullptr);
      auto const reopenedRender = reopenedTarget->renderPcm(output);
      REQUIRE(reopenedRender.bytesWritten == output.size());
      CHECK_FALSE(reopenedRender.drained);
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
      reopenedTarget->handlePositionAdvanced(reopenedRender.positionFrames);
      CHECK(engine.status().routeState.sourceChannelLayout ==
            ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified});
    }

    auto const status = engine.status();
    CHECK(status.transport == Transport::Playing);
    CHECK(status.routeState.sourceFormat.speakerMask == nextMask);
    CHECK(status.routeState.decoderOutputFormat.speakerMask == nextMask);
    CHECK(status.routeState.engineOutputFormat.speakerMask == nextMask);
    CHECK(backend->currentFormat().speakerMask == nextMask);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) ==
          (nextMask == 0x33 ? 1 : 2));
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) ==
          (nextMask == 0x33 ? 1 : 2));
    CHECK(std::ranges::count(events, std::string{"close"}, &FakeCapturingBackend::Event::name) ==
          (nextMask == 0x33 ? 0 : 1));

    auto const lock = std::scoped_lock{callbackMutex};

    if (nextMask == 0x33)
    {
      REQUIRE(advancedEvents.size() == 1);
      CHECK(advancedEvents.front().itemId == secondItem.id);
      CHECK(advancedEvents.front().input.filePath == secondFile.path);
      CHECK(advancedEvents.front().generation == engine.routeStatus().generation);
      CHECK(endedEvents.empty());
    }
    else
    {
      CHECK(advancedEvents.empty());
      REQUIRE(endedEvents.size() == 1);
      CHECK(endedEvents.front().generation == firstGeneration);
    }
  }

  TEST_CASE("Engine - real partial WAV successor refusal clears armed lookahead without interrupting current playback",
            "[audio][integration][engine][wav][concurrency]")
  {
    auto const currentPcm = std::vector<std::uint8_t>{0x11, 0x12, 0x21, 0x22, 0x31, 0x32, 0x41, 0x42};
    auto const currentBytes = ao::test::wav::makeWav({.channels = 2, .audioData = currentPcm});
    auto const currentFile = ao::test::TempFile{currentBytes, ".wav"};
    auto const successorBytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                        .channels = 2,
                                                        .channelMask = 0x3,
                                                        .audioData = {0x51, 0x52, 0x61, 0x62, 0x71, 0x72, 0x81, 0x82}});
    auto const successorFile = ao::test::TempFile{successorBytes, ".wav"};
    auto const refusedBytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                      .channels = 2,
                                                      .channelMask = 0x1,
                                                      .audioData = {0x91, 0x92, 0xA1, 0xA2}});
    auto const refusedFile = ao::test::TempFile{refusedBytes, ".wav"};
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
    CHECK(refusedRes.error().code == Error::Code::NotSupported);
    CHECK(refusedRes.error().message.contains("WAV channel layout"));
    CHECK(refusedRes.error().message.contains("partially"));

    // Inspection refusal returns through Result, unlike play()'s failure callback path.
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

    auto output = std::array<std::byte, 8>{};
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

  TEST_CASE("Engine - real partial WAV declaration is a recoverable source refusal before backend open",
            "[audio][integration][engine][wav]")
  {
    auto const bytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                               .channels = 2,
                                               .channelMask = 0x1,
                                               .audioData = {0x11, 0x12, 0x21, 0x22}});
    auto const file = ao::test::TempFile{bytes, ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    auto failureFuture = captureNextFailure(engine);
    auto const item = makePlaybackItem(file.path);
    engine.play(item);
    auto const failure = requireFailure(failureFuture);
    CHECK(failure.itemId == item.id);
    CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
    CHECK(failure.recoverable);
    CHECK(failure.error.code == Error::Code::NotSupported);
    CHECK(failure.error.message.contains("WAV channel layout"));
    CHECK(failure.error.message.contains("partially"));
    CHECK(engine.status().transport == Transport::Idle);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(events, std::string{"open-failed"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 0);
  }
} // namespace ao::audio::test
