// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <semaphore>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // A backend whose stop() synchronously delivers onDrainComplete to the
    // render target, modeling a drain callback that is already in flight when a
    // control command stops the stream.
    class DrainOnStopBackend final : public IBackend
    {
    public:
      Result<> open(Format const& /*format*/, IRenderTarget* target) override
      {
        _target = target;
        ++_openCount;
        return {};
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
          _target->onDrainComplete();
        }
      }

      void close() override
      {
        ++_closeCount;
        _target = nullptr;
      }

      BackendId backendId() const noexcept override { return BackendId{"drain-on-stop"}; }
      ProfileId profileId() const noexcept override { return ProfileId{"test"}; }
      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

      Result<PropertyValue> property(PropertyId /*id*/) const override
      {
        return std::unexpected(Error{.code = Error::Code::NotSupported});
      }

      PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override { return {}; }

      IRenderTarget* target() const { return _target; }
      std::size_t openCount() const { return _openCount; }
      std::size_t closeCount() const { return _closeCount; }

    private:
      IRenderTarget* _target = nullptr;
      std::size_t _openCount = 0;
      std::size_t _stopCount = 0;
      std::size_t _closeCount = 0;
    };
  } // namespace

  TEST_CASE("Engine - drain transitions idle and notifies track end", "[audio][unit][engine][drain]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    auto const fmt = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(20, std::byte{0}); // 10ms

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    auto trackEnded = std::atomic<bool>{false};
    engine.setOnTrackEnded([&] { trackEnded.store(true, std::memory_order_release); });

    engine.play(makePlaybackItem(desc));

    // Simulate playback loop via backend callbacks
    auto* const target = backendRaw->target();
    auto buffer = std::array<std::byte, 100>{};

    std::ignore = target->renderPcm(buffer).bytesWritten; // Read all 20 bytes
    CHECK(target->renderPcm(buffer).drained);

    SECTION("onDrainComplete resets to idle and fires track ended")
    {
      auto trackEndedLatch = CallbackLatch{};
      engine.setOnTrackEnded([&] { trackEndedLatch.notify(); });

      backendRaw->fireDrainComplete();
      CHECK(trackEndedLatch.waitForCount(1));
      CHECK(engine.status().transport == Transport::Idle);
    }

    SECTION("onDrainComplete without pending drain is ignored")
    {
      engine.stop(); // resets everything
      trackEnded.store(false, std::memory_order_release);
      backendRaw->fireDrainComplete();
      CHECK_FALSE(trackEnded.load(std::memory_order_acquire));
    }

    SECTION("onBackendError stops playback")
    {
      auto stateChanged = CallbackLatch{};
      engine.setOnStateChanged([&] { stateChanged.notify(); });

      backendRaw->fireBackendError("lost device");
      CHECK(stateChanged.waitForCount(1));
      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText == "lost device");
    }
  }

  TEST_CASE("Engine - play ignores stale pending drain from retired session", "[audio][unit][engine-drain][window]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto const secondData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};

    auto endedLatch = CallbackLatch{};
    auto routeEntered = CallbackLatch{};
    auto secondRouteLatch = CallbackLatch{};
    auto releaseRoute = std::binary_semaphore{0};
    auto parkOnce = std::atomic<bool>{true};

    auto engine = Engine{std::move(backendPtr),
                         device,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};

    engine.setOnTrackEnded([&] { endedLatch.notify(); });
    engine.setOnRouteChanged(
      [&](Engine::RouteStatus const& route)
      {
        if (parkOnce.exchange(false))
        {
          routeEntered.notify();
          std::ignore = releaseRoute.try_acquire_for(std::chrono::seconds{2});
        }

        if (route.optAnchor && route.optAnchor->id == "second-anchor")
        {
          secondRouteLatch.notify();
        }
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    auto* target = backendRaw->target();
    REQUIRE(target != nullptr);

    backendRaw->fireRouteReady("first-anchor");
    REQUIRE(routeEntered.waitForCount(1));

    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(target->renderPcm(out).drained);
    backendRaw->fireDrainComplete();

    // play() retires the drained render session while the event worker is still
    // parked. The old drain signal must not later surface as onTrackEnded for
    // the newly started track.
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "second.flac"}));
    target = backendRaw->target();
    REQUIRE(target != nullptr);
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == secondData);

    backendRaw->fireRouteReady("second-anchor");
    releaseRoute.release();
    REQUIRE(secondRouteLatch.waitForCount(1));
    CHECK(endedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - seek ignores stale pending drain and keeps session playing", "[audio][unit][engine-seek][drain]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const initialData = std::vector{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    auto const seekData = std::vector{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}};

    auto endedLatch = CallbackLatch{};
    auto routeEntered = CallbackLatch{};
    auto afterSeekRouteLatch = CallbackLatch{};
    auto releaseRoute = std::binary_semaphore{0};
    auto parkOnce = std::atomic<bool>{true};

    auto engine =
      Engine{std::move(backendPtr),
             device,
             makeRegisteringDecoderFactory(
               {
                 {.track = {.path = "track.flac", .info = makeScriptedStreamInfo(format), .data = initialData},
                  .optSeekScript = std::vector<ScriptedDecoderSession::ReadScriptEntry>{{seekData, false}, {{}, true}}},
               },
               std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>())};

    engine.setOnTrackEnded([&] { endedLatch.notify(); });
    engine.setOnRouteChanged(
      [&](Engine::RouteStatus const& route)
      {
        if (parkOnce.exchange(false))
        {
          routeEntered.notify();
          std::ignore = releaseRoute.try_acquire_for(std::chrono::seconds{2});
        }

        if (route.optAnchor && route.optAnchor->id == "after-seek")
        {
          afterSeekRouteLatch.notify();
        }
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "track.flac"}));
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    backendRaw->fireRouteReady("before-seek");
    REQUIRE(routeEntered.waitForCount(1));

    auto out = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == initialData);
    CHECK(target->renderPcm(out).drained);
    backendRaw->fireDrainComplete();

    // seek() keeps the same render session, so generation alone cannot identify
    // the old drain completion as stale. It must cancel that drain before
    // repositioning the source.
    engine.seek(std::chrono::milliseconds{2});
    REQUIRE(target->renderPcm(out).bytesWritten == out.size());
    CHECK(std::vector<std::byte>{out.begin(), out.end()} == seekData);

    backendRaw->fireRouteReady("after-seek");
    releaseRoute.release();
    REQUIRE(afterSeekRouteLatch.waitForCount(1));
    CHECK(endedLatch.count() == 0);
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - seek landing at end of stream retires the render session before quiescing",
            "[audio][unit][engine-seek][drain]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<DrainOnStopBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
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

    engine.setOnTrackEnded([&] { endedLatch.notify(); });

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

    // Seek while the drain is in flight: the backend delivers onDrainComplete
    // inside the stop() this seek issues, so a Drained signal for the current
    // generation lands in the ring mid-command. The post-seek script is empty,
    // so the seek lands at end of stream and takes the quiesce path.
    engine.seek(std::chrono::milliseconds{5});
    CHECK(engine.status().transport == Transport::Idle);

    // The quiesce path retired the render session, so the in-flight drain
    // signal is inert: no spurious track-end, no second quiesce of the closed
    // backend.
    CHECK_FALSE(endedLatch.waitForCount(1, std::chrono::milliseconds{200}));
    CHECK(engine.status().transport == Transport::Idle);
    CHECK(backendRaw->closeCount() == closesAfterPlay + 1);
  }
} // namespace ao::audio::test
