// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <semaphore>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // A backend whose stop() synchronously delivers handleDrainComplete to the
    // render target, modeling a drain callback that is already in flight when a
    // control command stops the stream.
    class DrainOnStopBackend final : public Backend
    {
    public:
      Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& target) override
      {
        _target = &target;
        ++_openCount;
        return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, SampleEncoding::Signed16Le)};
      }

      void start() override {}
      void pause() override {}
      void resume() override {}
      void flush() override {}

      void stop() override
      {
        ++_stopCount;

        if (_target != nullptr)
        {
          _target->handleDrainComplete();
        }
      }

      void close() override
      {
        ++_closeCount;
        _target = nullptr;
      }

      BackendId backendId() const override { return BackendId{"drain-on-stop"}; }
      ProfileId profileId() const override { return ProfileId{"test"}; }
      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

      Result<PropertyValue> property(PropertyId /*id*/) const override
      {
        return std::unexpected(Error{.code = Error::Code::NotSupported});
      }

      PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override { return {}; }

      RenderTarget* target() const { return _target; }
      std::size_t openCount() const { return _openCount; }
      std::size_t closeCount() const { return _closeCount; }

    private:
      RenderTarget* _target = nullptr;
      std::size_t _openCount = 0;
      std::size_t _stopCount = 0;
      std::size_t _closeCount = 0;
    };
  } // namespace

  TEST_CASE("Engine - drain completion requires a pending render drain", "[audio][unit][engine][drain][concurrency]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    auto const fmt = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const factory = [fmt](auto const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = signalFormat(fmt);
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = sourceFormat,
                          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(fmt.encoding)),
                          .duration = std::chrono::milliseconds{0},
                          .isLossy = false});
      auto data = std::vector(20, std::byte{0}); // 10ms

      decPtr->setReadScript({{.data = data, .endOfStream = false}, {.endOfStream = true}});
      return decPtr;
    };

    auto endedLatch = CallbackLatch{};
    auto settledLatch = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), device, factory};
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });
    engine.play(makePlaybackItem("song.flac"));
    REQUIRE(engine.status().transport == Transport::Playing);
    REQUIRE(backendRaw->target() != nullptr);

    if (auto const signalBeforeDrain = GENERATE(false, true); signalBeforeDrain)
    {
      backendRaw->emitDrainComplete();
      // A control command settles RT signals ahead of the deferred barrier.
      REQUIRE(engine.setVolume(0.5F));
      engine.defer([&] { settledLatch.notify(); });
      REQUIRE(settledLatch.tryWaitForCount(1));
      CHECK(endedLatch.count() == 0);
      REQUIRE(engine.status().transport == Transport::Playing);
    }

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);
    auto buffer = std::array<std::byte, 100>{};
    REQUIRE(target->renderPcm(buffer).bytesWritten == 20);
    REQUIRE(target->renderPcm(buffer).drained);
    backendRaw->emitDrainComplete();
    REQUIRE(endedLatch.tryWaitForCount(1));
    CHECK(endedLatch.count() == 1);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - an already drained prepared source completes without starting the backend",
            "[audio][unit][engine][drain]")
  {
    auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const factory = [format](auto const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = signalFormat(format);
      return std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = sourceFormat,
                          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(format.encoding)),
                          .duration = std::chrono::milliseconds{0},
                          .isLossy = false});
    };
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto endedLatch = CallbackLatch{};
    auto endedGeneration = std::atomic<std::uint64_t>{0};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), factory};
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        endedGeneration.store(event.generation, std::memory_order_release);
        endedLatch.notify();
      });

    auto stagedRes = engine.stagePlayback(makePlaybackItem("empty.flac"));
    REQUIRE(stagedRes);
    auto committedRes = engine.commitPlayback(std::move(*stagedRes));

    REQUIRE(committedRes);
    CHECK_FALSE(committedRes->playbackStarted);
    REQUIRE(endedLatch.tryWaitForCount(1));
    CHECK(endedGeneration.load(std::memory_order_acquire) == committedRes->generation);
    CHECK(engine.status().transport == Transport::Idle);
    auto const events = backendRaw->events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].name == "open");
    CHECK(events[1].name == "stop");
    CHECK(events[2].name == "close");
  }

  TEST_CASE("Engine - play ignores stale pending drain from retired session",
            "[audio][unit][engine][drain][concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};

    auto endedLatch = CallbackLatch{};
    auto firstRouteLatch = CallbackLatch{};
    auto secondRouteLatch = CallbackLatch{};
    auto workerEntered = CallbackLatch{};
    auto releaseWorker = std::binary_semaphore{0};

    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};
    auto releaseGuard = utility::ScopedRegistration{[&] { releaseWorker.release(); }};

    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });
    engine.setOnRouteChanged(
      [&](Engine::RouteStatus const& route)
      {
        if (route.optAnchor && route.optAnchor->id == "first-anchor")
        {
          firstRouteLatch.notify();
        }
        else if (route.optAnchor && route.optAnchor->id == "second-anchor")
        {
          secondRouteLatch.notify();
        }
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto* target = backendRaw->target();
    REQUIRE(target != nullptr);

    backendRaw->emitRouteReady("first-anchor");
    REQUIRE(firstRouteLatch.tryWaitForCount(1));

    // A deferred task parks event delivery without holding the route callback
    // barrier that play() must cross before returning.
    engine.defer(
      [&]
      {
        workerEntered.notify();
        releaseWorker.acquire();
      });
    REQUIRE(workerEntered.tryWaitForCount(1));

    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(target->renderPcm(out).drained);
    backendRaw->emitDrainComplete();

    // play() retires the drained render session while the event worker is still
    // parked. The old drain signal must not later surface as onTrackEnded for
    // the newly started track.
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "second.flac"}));
    target = backendRaw->target();
    REQUIRE(target != nullptr);
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);

    backendRaw->emitRouteReady("second-anchor");
    releaseGuard.reset();
    REQUIRE(secondRouteLatch.tryWaitForCount(1));
    CHECK(endedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - seek ignores stale pending drain and keeps session playing",
            "[audio][unit][engine][seek][drain][concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const initialData = std::vector{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    auto const seekData = std::vector{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}};

    auto endedLatch = CallbackLatch{};
    auto beforeSeekRouteLatch = CallbackLatch{};
    auto afterSeekRouteLatch = CallbackLatch{};
    auto workerEntered = CallbackLatch{};
    auto releaseWorker = std::binary_semaphore{0};

    auto engine =
      Engine{std::move(backendPtr),
             device,
             makeRegisteringDecoderFactory(
               {
                 {.track = {.path = "track.flac", .info = makeScriptedStreamInfo(format), .data = initialData},
                  .optSeekScript =
                    std::vector<ScriptedDecoderSession::ReadScriptEntry>{
                      {.data = seekData, .endOfStream = false}, {.endOfStream = true}}},
               },
               std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>())};
    auto releaseGuard = utility::ScopedRegistration{[&] { releaseWorker.release(); }};

    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });
    engine.setOnRouteChanged(
      [&](Engine::RouteStatus const& route)
      {
        if (route.optAnchor && route.optAnchor->id == "before-seek")
        {
          beforeSeekRouteLatch.notify();
        }
        else if (route.optAnchor && route.optAnchor->id == "after-seek")
        {
          afterSeekRouteLatch.notify();
        }
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "track.flac"}));
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    backendRaw->emitRouteReady("before-seek");
    REQUIRE(beforeSeekRouteLatch.tryWaitForCount(1));
    engine.defer(
      [&]
      {
        workerEntered.notify();
        releaseWorker.acquire();
      });
    REQUIRE(workerEntered.tryWaitForCount(1));

    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == initialData);
    CHECK(target->renderPcm(out).drained);
    backendRaw->emitDrainComplete();

    // seek() keeps the same render session, so generation alone cannot identify
    // the old drain completion as stale. It must cancel that drain before
    // repositioning the source.
    engine.seek(std::chrono::milliseconds{2});
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == seekData);

    backendRaw->emitRouteReady("after-seek");
    releaseGuard.reset();
    REQUIRE(afterSeekRouteLatch.tryWaitForCount(1));
    CHECK(endedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - seek landing at end of stream completes the track and retires the render session",
            "[audio][unit][engine][seek][drain][concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<DrainOnStopBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const data = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};

    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();
    auto endedLatch = CallbackLatch{};

    auto engine = Engine{std::move(backendPtr),
                         device,
                         makeRegisteringDecoderFactory(
                           {
                             {.track = {.path = "track.flac", .info = makeScriptedStreamInfo(format), .data = data},
                              .optSeekScript = std::vector<ScriptedDecoderSession::ReadScriptEntry>{}},
                           },
                           registryPtr)};

    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedLatch.notify(); });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "track.flac"}));
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    // play() itself closes any previous stream, so measure the quiesce below
    // against this baseline.
    auto const closesAfterPlay = backendRaw->closeCount();

    // Consume the track and let the drain fallback engage (no successor armed).
    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(target->renderPcm(out).drained);

    // Seek while the drain is in flight: the backend delivers handleDrainComplete
    // inside the stop() this seek issues, so a Drained signal for the current
    // generation lands in the ring mid-command. The post-seek script is empty,
    // so the seek lands at end of stream and takes the quiesce path.
    engine.seek(std::chrono::milliseconds{5});
    CHECK(engine.status().transport == Transport::Idle);

    // The quiesce path retired the render session, so the in-flight drain
    // signal is inert: only the seek's synchronous natural-completion path may
    // publish track end, and it quiesces the backend exactly once.
    REQUIRE(endedLatch.tryWaitForCount(1));
    CHECK(endedLatch.count() == 1);
    CHECK(engine.status().transport == Transport::Idle);
    CHECK(backendRaw->closeCount() == closesAfterPlay + 1);
  }
} // namespace ao::audio::test
