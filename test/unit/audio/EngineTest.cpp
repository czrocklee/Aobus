// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "TestUtility.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/Error.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    class CallbackLatch final
    {
    public:
      void notify()
      {
        auto const lock = std::scoped_lock{_mutex};
        ++_count;
        _cv.notify_all();
      }

      bool waitForCount(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds{1})
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, timeout, [this, expected] { return _count >= expected; });
      }

      std::size_t count() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _count;
      }

    private:
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      std::size_t _count = 0;
    };
  } // namespace

  using namespace fakeit;

  TEST_CASE("Engine - stop closes backend and leaves transport idle", "[audio][unit][engine]")
  {
    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto engine = Engine{spy.makeProxy(), device};

    engine.stop();
    Verify(Method(mockBackend, stop)).AtLeastOnce();
    Verify(Method(mockBackend, close)).AtLeastOnce();

    auto snap = engine.status();
    CHECK(snap.transport == Transport::Idle);
  }

  TEST_CASE("Engine - setBackend while idle closes old backend and updates status", "[audio][unit][engine][hot-swap]")
  {
    auto spy1 = SpyBackend<>{};
    auto spy2 = SpyBackend<>{};
    auto& mockBackend1 = spy1.mock();
    auto& mockBackend2 = spy2.mock();

    When(Method(mockBackend1, backendId)).AlwaysReturn(kBackendNone);
    When(Method(mockBackend1, profileId)).AlwaysReturn(kProfileShared);

    When(Method(mockBackend2, backendId)).AlwaysReturn(kBackendAlsa);
    When(Method(mockBackend2, profileId)).AlwaysReturn(kProfileExclusive);

    auto engine = Engine{spy1.makeProxy(),
                         {.id = DeviceId{"dev1"},
                          .displayName = "D1",
                          .description = "D1",
                          .isDefault = false,
                          .backendId = kBackendNone}};

    SECTION("Switching backend while idle")
    {
      engine.setBackend(spy2.makeProxy(),
                        {.id = DeviceId{"dev2"},
                         .displayName = "D2",
                         .description = "D2",
                         .isDefault = false,
                         .backendId = kBackendAlsa});

      Verify(Method(mockBackend1, stop)).Once();
      Verify(Method(mockBackend1, close)).Once();

      auto const snap = engine.status();
      CHECK(snap.backendId == kBackendAlsa);
      CHECK(snap.currentDeviceId == "dev2");
    }
  }

  TEST_CASE("Engine - play publishes route state from decoder stream info", "[audio][unit][engine][graph]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    auto spy = SpyBackend<>{};
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto engine = Engine{spy.makeProxy(), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(descriptor);

    auto const snap = engine.status();

    SECTION("AudioRouteFormatState reports decoder source format")
    {
      CHECK(snap.routeState.sourceFormat.sampleRate == 44100);
      CHECK(snap.routeState.sourceFormat.channels == 2);
      CHECK(snap.routeState.sourceFormat.bitDepth == 16);
    }

    SECTION("AudioRouteFormatState reports engine output format")
    {
      CHECK(snap.routeState.engineOutputFormat.sampleRate == 44100);
      CHECK(snap.routeState.engineOutputFormat.channels == 2);
    }

    SECTION("AudioRouteFormatState reports lossless source")
    {
      CHECK(snap.routeState.isLossySource == false);
    }

    engine.stop();
  }

  TEST_CASE("Engine - PipeWire shared mode keeps native sample rate", "[audio][unit][engine][pipewire]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"pipewire-shared"},
                               .displayName = "PipeWire",
                               .description = "PipeWire Shared",
                               .isDefault = false,
                               .backendId = kBackendPipeWire,
                               .capabilities = {.sampleRates = {48000},
                                                .sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false}},
                                                .bitDepths = {16},
                                                .channelCounts = {2}}};

    auto openedFormats = std::vector<Format>{};

    When(Method(mockBackend, open))
      .AlwaysDo(
        [&](Format const& format, IRenderTarget*)
        {
          openedFormats.push_back(format);
          return Result<>{};
        });
    When(Method(mockBackend, backendId)).AlwaysReturn(kBackendPipeWire);

    auto engine = Engine{spy.makeProxy(), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(descriptor);

    REQUIRE(!openedFormats.empty());
    CHECK(openedFormats.back().sampleRate == 44100);
    CHECK(openedFormats.back().channels == 2);
    CHECK(openedFormats.back().bitDepth == 16);
    CHECK(engine.status().transport == Transport::Playing);

    engine.stop();
  }

  TEST_CASE("Engine - AAC playback supports 32-bit padded backend output", "[audio][unit][engine][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const device =
      Device{.id = DeviceId{"alsa-exclusive"},
             .displayName = "ALSA",
             .description = "ALSA Exclusive",
             .isDefault = false,
             .backendId = kBackendAlsa,
             .capabilities = {.sampleRates = {44100}, .sampleFormats = {}, .bitDepths = {32}, .channelCounts = {2}}};

    auto engine = Engine{std::move(backendPtr), device};
    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(descriptor);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Playing);

    auto const events = backendRaw->events();
    REQUIRE(!events.empty());
    auto optOpenFormat = std::optional<Format>{};

    for (auto const& event : events)
    {
      if (event.name == "open")
      {
        optOpenFormat = event.format;
        break;
      }
    }

    REQUIRE(optOpenFormat);
    CHECK(optOpenFormat->bitDepth == 32);
    CHECK(optOpenFormat->validBits == 16);
    CHECK(optOpenFormat->sampleRate == 44100);

    engine.stop();
  }

  TEST_CASE("Engine - Unsupported backend sample rate fails without resampler", "[audio][unit][engine][format]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"alsa-exclusive"},
                               .displayName = "ALSA",
                               .description = "ALSA Exclusive",
                               .isDefault = false,
                               .backendId = kBackendAlsa,
                               .capabilities = {.sampleRates = {48000},
                                                .sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false}},
                                                .bitDepths = {16},
                                                .channelCounts = {2}}};

    auto openedFormats = std::vector<Format>{};

    When(Method(mockBackend, open))
      .AlwaysDo(
        [&](Format const& format, IRenderTarget*)
        {
          openedFormats.push_back(format);
          return Result<>{};
        });
    When(Method(mockBackend, backendId)).AlwaysReturn(kBackendAlsa);
    When(Method(mockBackend, profileId)).AlwaysReturn(kProfileExclusive);

    auto engine = Engine{spy.makeProxy(), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(descriptor);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
    CHECK(openedFormats.empty());
  }

  TEST_CASE("Engine - play reports decoder and backend setup failures", "[audio][unit][engine][error]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    SECTION("Unsupported extension")
    {
      auto engine = Engine{std::make_unique<CapturingBackend>(), device};
      auto const desc = PlaybackInput{.filePath = "song.txt"};

      engine.play(desc);

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText.find("Unsupported audio file extension") != std::string::npos);
    }

    SECTION("Decoder open failure")
    {
      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});

        decPtr->setOpenResult(std::unexpected(Error{.message = "open failed"}));
        return decPtr;
      };

      auto engine = Engine{std::make_unique<CapturingBackend>(), device, factory};
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText == "open failed");
    }

    SECTION("Backend open failure")
    {
      auto backendPtr = std::make_unique<CapturingBackend>();

      backendPtr->setOpenResult(std::unexpected(Error{.message = "hw init failed"}));

      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = fmt,
          .outputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true},
          .duration = std::chrono::milliseconds{0},
          .isLossy = false});

        decPtr->setReadScript({{{}, true}});
        return decPtr;
      };

      auto engine = Engine{std::move(backendPtr), device, factory};
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText == "hw init failed");
    }
  }

  TEST_CASE("Engine - pause and resume update backend transport", "[audio][unit][engine][transport]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});

      // provide some data for preroll
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    engine.play(desc);
    CHECK(engine.status().transport == Transport::Playing);

    SECTION("Pause from Playing")
    {
      engine.pause();
      CHECK(engine.status().transport == Transport::Paused);
      CHECK(backendRaw->events().back().name == "pause");
    }

    SECTION("Resume from Paused")
    {
      engine.pause();
      backendRaw->clearEvents();
      engine.resume();
      CHECK(engine.status().transport == Transport::Playing);
      CHECK(backendRaw->events().back().name == "resume");
    }
  }

  TEST_CASE("Engine - seek updates elapsed time only after playback starts", "[audio][unit][engine][seek]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();

    auto const fmt = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true}; // 2 bytes = 1ms
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(200, std::byte{0}); // 100ms

      decPtr->setReadScript({{data, false}, {data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    SECTION("Seek before play is no-op")
    {
      engine.seek(std::chrono::milliseconds{100});
      CHECK(engine.status().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("Active seek success")
    {
      engine.play(desc);
      engine.seek(std::chrono::milliseconds{50});
      CHECK(engine.status().elapsed == std::chrono::milliseconds{50});
      CHECK(engine.status().transport == Transport::Playing);
    }
  }

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

    engine.play(desc);

    // Simulate playback loop via backend callbacks
    auto* const target = backendRaw->target();
    auto buffer = std::array<std::byte, 100>{};

    target->readPcm(buffer); // Read all 20 bytes

    CHECK(target->isSourceDrained());

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

  TEST_CASE("Engine - source decode error transitions to Error and ends track", "[audio][unit][engine][error]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::seconds{1}, .isLossy = false});

      // First block succeeds (preroll), second block fails
      // 100,000 bytes at 44.1kHz stereo 16-bit is ~566ms, satisfying the 500ms preroll
      decPtr->setReadScript(
        {{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false},
         {.data = {}, .endOfStream = false, .result = std::unexpected(Error{.message = "decode failed"})}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "fail.flac"};

    auto errorPromise = std::promise<void>{};
    auto errorFuture = errorPromise.get_future();
    engine.setOnTrackEnded([&errorPromise] { errorPromise.set_value(); });

    engine.play(desc);

    // The StreamingSource decode loop runs in a background thread.
    // It should hit the error and call handleSourceError, which now
    // fires onTrackEnded so we can synchronize without polling.
    CHECK(errorFuture.wait_for(std::chrono::seconds{15}) == std::future_status::ready);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "decode failed");
  }
} // namespace ao::audio::test
