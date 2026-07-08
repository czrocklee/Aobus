// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <semaphore>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    std::size_t countBackendEvents(std::vector<CapturingBackend::Event> const& events, std::string_view name)
    {
      std::size_t count = 0;

      for (auto const& event : events)
      {
        if (event.name == name)
        {
          ++count;
        }
      }

      return count;
    }
  } // namespace

  TEST_CASE("Engine - splices prepared lossless same-format track without restarting backend",
            "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    auto advancedPath = std::filesystem::path{};
    auto advancedItemId = Engine::PlaybackItemId{};
    engine.setOnTrackAdvanced(
      [&](Engine::TrackAdvanced const& event)
      {
        advancedPath = event.input.filePath;
        advancedItemId = event.itemId;
        advancedLatch.notify();
      });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto const secondItem = makePlaybackItem(PlaybackInput{.filePath = "second.flac"});
    auto const prepared = engine.setNext(secondItem);
    REQUIRE(prepared);
    CHECK(prepared->itemId == secondItem.id);
    CHECK(prepared->transition == Engine::PreparedTransitionMode::Gapless);

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    target->onUnderrun();
    CHECK(engine.status().underrunCount == 1);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(std::vector<std::byte>{firstOut.begin(), firstOut.end()} == firstData);

    auto secondOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(secondOut).bytesWritten == secondOut.size());
    CHECK(std::vector<std::byte>{secondOut.begin(), secondOut.end()} == secondData);
    REQUIRE(advancedLatch.waitForCount(1));
    CHECK(advancedPath == "second.flac");
    CHECK(advancedItemId == secondItem.id);
    CHECK(engine.status().underrunCount == 0);
    CHECK(endedLatch.count() == 0);

    auto const events = backendRaw->events();
    CHECK(countBackendEvents(events, "open") == 1);
    CHECK(countBackendEvents(events, "start") == 1);
  }

  TEST_CASE("Engine - cross-boundary gapless render advances position only for successor frames",
            "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21},
                                        std::byte{0x22},
                                        std::byte{0x23},
                                        std::byte{0x24},
                                        std::byte{0x25},
                                        std::byte{0x26},
                                        std::byte{0x27},
                                        std::byte{0x28}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto out = std::array<std::byte, 8>{};
    auto const renderResult = target->renderPcm(out);

    REQUIRE(renderResult.bytesWritten == out.size());
    CHECK(renderResult.positionFrameOffset == 2);
    CHECK(renderResult.positionFrames == 2);
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == std::vector<std::byte>{std::byte{0x11},
                                                                                   std::byte{0x12},
                                                                                   std::byte{0x13},
                                                                                   std::byte{0x14},
                                                                                   std::byte{0x21},
                                                                                   std::byte{0x22},
                                                                                   std::byte{0x23},
                                                                                   std::byte{0x24}});

    target->onPositionAdvanced(renderResult.positionFrames);

    REQUIRE(advancedLatch.waitForCount(1));
    CHECK(engine.status().elapsed == std::chrono::milliseconds{2});
  }

  TEST_CASE("Engine - consecutive gapless splices reclaim retired sources without restarting backend",
            "[audio][unit][engine-gapless][reclaim]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
    auto const thirdData = std::vector{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};

    auto countersPtr = std::make_shared<DecoderLifeCounters>();
    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makeCountingDecoderFactory(
                           {
                             {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                             {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                             {.path = "third.flac", .info = makeScriptedStreamInfo(format), .data = thirdData},
                           },
                           countersPtr)};

    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    // First playing, second armed: exactly two live streaming sources.
    CHECK(countersPtr->live() == 2);

    auto out = std::array<std::byte, 4>{};

    // First -> second splice.
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == firstData);
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);
    REQUIRE(advancedLatch.waitForCount(1));
    // The first (retired) source has been destroyed off the render thread; only
    // the now-current second source remains live (no accumulation).
    CHECK(countersPtr->live() == 1);

    // Arm the next successor only after the advance is observed, mirroring how
    // the queue prepares N+2 in response to N+1 becoming current. This also
    // proves the event thread refreshed the current-track format so the gate
    // sees the now-playing track.
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "third.flac"})));
    CHECK(countersPtr->live() == 2);

    // Second -> third splice.
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == thirdData);
    REQUIRE(advancedLatch.waitForCount(2));
    CHECK(countersPtr->live() == 1);
    CHECK(endedLatch.count() == 0);

    // The backend was opened and started exactly once across the whole chain.
    auto const events = backendRaw->events();
    CHECK(countBackendEvents(events, "open") == 1);
    CHECK(countBackendEvents(events, "start") == 1);
  }

  TEST_CASE("Engine - stop frees an armed but unspliced successor", "[audio][unit][engine-gapless][cancel]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};

    auto countersPtr = std::make_shared<DecoderLifeCounters>();
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makeCountingDecoderFactory(
                           {
                             {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                             {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                           },
                           countersPtr)};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));
    CHECK(countersPtr->live() == 2);

    // Stop before the boundary: the current source and the armed-but-unspliced
    // successor are both released synchronously, so neither leaks.
    engine.stop();
    CHECK(countersPtr->live() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - control command entry settles a pending splice before acting",
            "[audio][unit][engine-gapless][window]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};

    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();

    // Declared before the engine so they outlive the event thread's join.
    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    auto routeEntered = CallbackLatch{};
    auto releaseRoute = std::binary_semaphore{0};
    auto parkOnce = std::atomic<bool>{true};

    auto engine =
      Engine{std::move(backendPtr),
             device,
             makeRegisteringDecoderFactory(
               {
                 {.track = {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                  .optSeekScript = std::nullopt},
                 {.track = {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                  .optSeekScript = std::nullopt},
               },
               registryPtr)};

    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    // Parks the event worker inside a notification exactly once, so the splice
    // signal produced below stays unapplied in the ring until a control command
    // settles it. The wait is bounded so a failing test unwinds instead of
    // deadlocking the worker join.
    engine.setOnRouteChanged(
      [&](Engine::RouteStatus const&)
      {
        if (parkOnce.exchange(false))
        {
          routeEntered.notify();
          std::ignore = releaseRoute.try_acquire_for(std::chrono::seconds{2});
        }
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    backendRaw->emitRouteReady("anchor");
    REQUIRE(routeEntered.waitForCount(1));

    // Drive the render side (this thread stands in for the RT thread): consume
    // the first track and splice into the second.
    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);

    // The worker is parked, so the advance has not been applied yet: this is
    // exactly the raw-pointer-published / owner-not-promoted window.
    CHECK(advancedLatch.count() == 0);

    // seek() must settle the pending splice at entry and act on the new source,
    // not on the retired first track.
    engine.seek(std::chrono::milliseconds{3});

    auto* const secondDecoder = registryPtr->at("second.flac");
    CHECK(secondDecoder->lastSeekOffset() == std::chrono::milliseconds{3});
    CHECK(secondDecoder->seekCount() == 1);

    // Playback continues from the (re-wound) second source.
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);

    // The advance notification is still delivered by the event worker once it
    // is unparked, so observers see the usual callback thread and order.
    releaseRoute.release();
    REQUIRE(advancedLatch.waitForCount(1));
    CHECK(endedLatch.count() == 0);

    // The whole splice-plus-windowed-seek ran on the original backend stream.
    CHECK(countBackendEvents(backendRaw->events(), "open") == 1);
  }

  TEST_CASE("Engine - prepared lossy same-format track takes drain fallback", "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    auto const secondData = std::vector{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}};
    auto engine = Engine{
      std::move(backendPtr),
      device,
      makePathScriptedDecoderFactory({
        {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
        {.path = "second.mp3", .info = makeScriptedStreamInfo(format, AudioCodec::Mp3, true), .data = secondData},
      })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    CHECK(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.mp3"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - prepared unknown codec track takes drain fallback", "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}};
    auto const secondData = std::vector{std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}};
    auto engine = Engine{
      std::move(backendPtr),
      device,
      makePathScriptedDecoderFactory({
        {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
        {.path = "second.bin", .info = makeScriptedStreamInfo(format, AudioCodec::Unknown, false), .data = secondData},
      })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    CHECK(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.bin"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - prepared lossless format mismatch takes drain fallback", "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const firstFormat = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const secondFormat = Format{.sampleRate = 2000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}};
    auto const secondData = std::vector{std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(firstFormat), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(secondFormat), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    CHECK(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - transport and route changes cancel prepared successor", "[audio][unit][engine-gapless][cancel]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}};
    auto const secondData = std::vector{std::byte{0x59}, std::byte{0x5A}, std::byte{0x5B}, std::byte{0x5C}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    SECTION("seek")
    {
      engine.seek(std::chrono::milliseconds{0});
    }

    SECTION("updateDevice")
    {
      engine.updateDevice(makeEngineTestDevice("replacement-device"));
    }

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(std::vector<std::byte>{firstOut.begin(), firstOut.end()} == firstData);
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - clearNext before end of stream restores drain fallback", "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}};
    auto const secondData = std::vector{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}, std::byte{0x64}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto const secondItem = makePlaybackItem(PlaybackInput{.filePath = "second.flac"});
    REQUIRE(engine.setNext(secondItem));
    CHECK(engine.clearNext() == secondItem.id);

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - clearNext returns empty after render consumed the successor", "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x81}, std::byte{0x82}, std::byte{0x83}, std::byte{0x84}};
    auto const secondData = std::vector{std::byte{0x91}, std::byte{0x92}, std::byte{0x93}, std::byte{0x94}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    auto advancedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto const secondItem = makePlaybackItem(PlaybackInput{.filePath = "second.flac"});
    REQUIRE(engine.setNext(secondItem));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto out = std::array<std::byte, 8>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());

    CHECK_FALSE(engine.clearNext());
    REQUIRE(advancedLatch.waitForCount(1));
  }

  TEST_CASE("Engine - setNext reports prepare failure without installing a prepared track",
            "[audio][unit][engine][gapless]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x71}, std::byte{0x72}, std::byte{0x73}, std::byte{0x74}};
    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                         })};

    auto advancedLatch = CallbackLatch{};
    auto endedLatch = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advancedLatch.notify(); });
    engine.setOnTrackEnded([&] { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto const result = engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "missing.flac"}));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto firstOut = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(firstOut).bytesWritten == firstOut.size());
    CHECK(target->renderPcm(firstOut).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.waitForCount(1));
    CHECK(advancedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Idle);
  }
} // namespace ao::audio::test
