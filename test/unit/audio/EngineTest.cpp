// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "TestUtility.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/Types.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    template<typename Predicate>
    bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{1})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (std::chrono::steady_clock::now() < deadline)
      {
        if (predicate())
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      return predicate();
    }
  } // namespace

  using namespace fakeit;

  TEST_CASE("Engine - Basic Orchestration", "[playback][unit][engine]")
  {
    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto engine = Engine{spy.makeProxy(), device};

    SECTION("Stop correctly cleans up backend")
    {
      engine.stop();
      Verify(Method(mockBackend, stop)).AtLeastOnce();
      Verify(Method(mockBackend, close)).AtLeastOnce();

      auto snap = engine.status();
      REQUIRE(snap.transport == Transport::Idle);
    }

    SECTION("Volume and mute controls pass through to backend and update status")
    {
      auto lastSetPropertyId = PropertyId{};
      auto lastSetPropertyValue = PropertyValue{false};

      When(Method(mockBackend, setProperty))
        .AlwaysDo(
          [&](PropertyId id, PropertyValue const& value) -> Result<>
          {
            lastSetPropertyId = id;
            lastSetPropertyValue = value;
            return Result<>{};
          });

      engine.setVolume(0.75F);
      REQUIRE(lastSetPropertyId == PropertyId::Volume);
      REQUIRE(std::get<float>(lastSetPropertyValue) == Catch::Approx{0.75F});
      REQUIRE(engine.volume() == Catch::Approx{0.75F});
      REQUIRE(engine.status().volume == Catch::Approx{0.75F});

      engine.setMuted(true);
      REQUIRE(lastSetPropertyId == PropertyId::Muted);
      REQUIRE(std::get<bool>(lastSetPropertyValue) == true);
      REQUIRE(engine.isMuted() == true);
      REQUIRE(engine.status().muted == true);

      REQUIRE(engine.isVolumeAvailable() == true);
      REQUIRE(engine.status().volumeAvailable == true);
    }
  }

  TEST_CASE("Engine - Backend Swapping", "[playback][unit][engine][hot-swap]")
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
      REQUIRE(snap.backendId == kBackendAlsa);
      REQUIRE(snap.currentDeviceId == "dev2");
    }
  }

  TEST_CASE("Engine - Graph Initialization", "[playback][unit][engine][graph]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file not found: " << testFile);
    }

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

    SECTION("RouteState reports decoder source format")
    {
      CHECK(snap.routeState.sourceFormat.sampleRate == 44100);
      CHECK(snap.routeState.sourceFormat.channels == 2);
      CHECK(snap.routeState.sourceFormat.bitDepth == 16);
    }

    SECTION("RouteState reports engine output format")
    {
      CHECK(snap.routeState.engineOutputFormat.sampleRate == 44100);
      CHECK(snap.routeState.engineOutputFormat.channels == 2);
    }

    SECTION("RouteState reports lossless source")
    {
      CHECK(snap.routeState.isLossySource == false);
    }

    engine.stop();
  }

  TEST_CASE("Engine - PipeWire shared mode keeps native sample rate", "[playback][unit][engine][pipewire]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      WARN("Test file not found, skipping PipeWire shared format test");
      return;
    }

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

  TEST_CASE("Engine - AAC playback supports 32-bit padded backend output", "[playback][unit][engine][aac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

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
    REQUIRE(snap.transport == Transport::Playing);

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

  TEST_CASE("Engine - Unsupported backend sample rate fails without resampler", "[playback][unit][engine][format]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      WARN("Test file not found, skipping backend sample-rate validation test");
      return;
    }

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
    REQUIRE(snap.transport == Transport::Error);
    CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
    REQUIRE(openedFormats.empty());
  }

  TEST_CASE("Engine - Play failure matrix", "[playback][unit][engine][error]")
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

      REQUIRE(engine.status().transport == Transport::Error);
      REQUIRE(engine.status().statusText.find("Unsupported audio file extension") != std::string::npos);
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

      REQUIRE(engine.status().transport == Transport::Error);
      REQUIRE(engine.status().statusText == "open failed");
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

      REQUIRE(engine.status().transport == Transport::Error);
      REQUIRE(engine.status().statusText == "hw init failed");
    }
  }

  TEST_CASE("Engine - Pause and resume matrix", "[playback][unit][engine][transport]")
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
    REQUIRE(engine.status().transport == Transport::Playing);

    SECTION("Pause from Playing")
    {
      engine.pause();
      REQUIRE(engine.status().transport == Transport::Paused);
      REQUIRE(backendRaw->events().back().name == "pause");
    }

    SECTION("Resume from Paused")
    {
      engine.pause();
      backendRaw->clearEvents();
      engine.resume();
      REQUIRE(engine.status().transport == Transport::Playing);
      REQUIRE(backendRaw->events().back().name == "resume");
    }
  }

  TEST_CASE("Engine - Seek matrix", "[playback][unit][engine][seek]")
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
      REQUIRE(engine.status().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("Active seek success")
    {
      engine.play(desc);
      engine.seek(std::chrono::milliseconds{50});
      REQUIRE(engine.status().elapsed == std::chrono::milliseconds{50});
      REQUIRE(engine.status().transport == Transport::Playing);
    }
  }

  TEST_CASE("Engine - Drain and callback matrix", "[playback][unit][engine][drain]")
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

    REQUIRE(target->isSourceDrained());

    SECTION("onDrainComplete resets to idle and fires track ended")
    {
      backendRaw->fireDrainComplete();
      REQUIRE(waitUntil([&] { return engine.status().transport == Transport::Idle; }));
      REQUIRE(waitUntil([&] { return trackEnded.load(std::memory_order_acquire); }));
    }

    SECTION("onDrainComplete without pending drain is ignored")
    {
      engine.stop(); // resets everything
      trackEnded.store(false, std::memory_order_release);
      backendRaw->fireDrainComplete();
      REQUIRE_FALSE(
        waitUntil([&] { return trackEnded.load(std::memory_order_acquire); }, std::chrono::milliseconds{50}));
    }

    SECTION("onBackendError stops playback")
    {
      backendRaw->fireBackendError("lost device");
      REQUIRE(waitUntil([&] { return engine.status().transport == Transport::Error; }));
      CHECK(engine.status().statusText == "lost device");
    }
  }

  TEST_CASE("Engine - Property API", "[playback][unit][engine][property]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* backendRaw = backendPtr.get();

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::seconds{1}, .isLossy = false});
      decPtr->setReadScript({{.data = std::vector<std::byte>(88200, std::byte{0}), .endOfStream = false}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "test.flac"};

    SECTION("queryProperty returns all-false for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const info = backendRaw->queryProperty(kUnknownId);

      REQUIRE(info.canRead == false);
      REQUIRE(info.canWrite == false);
      REQUIRE(info.isAvailable == false);
      REQUIRE(info.emitsChangeNotifications == false);
    }

    SECTION("queryProperty returns valid info for Volume")
    {
      backendRaw->setMockPropertyInfo(PropertyId::Volume,
                                      PropertyInfo{
                                        .canRead = true,
                                        .canWrite = true,
                                        .isAvailable = true,
                                        .emitsChangeNotifications = false,
                                        .isHardwareAssisted = true,
                                      });

      auto const info = backendRaw->queryProperty(PropertyId::Volume);

      REQUIRE(info.canRead == true);
      REQUIRE(info.canWrite == true);
      REQUIRE(info.isAvailable == true);
      REQUIRE(info.isHardwareAssisted == true);
    }

    SECTION("setProperty returns error for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const result = backendRaw->setProperty(kUnknownId, PropertyValue{0.5F});

      REQUIRE(!result);
      REQUIRE(result.error().code == Error::Code::NotSupported);
    }

    SECTION("property returns error for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const result = backendRaw->property(kUnknownId);

      REQUIRE(!result);
      REQUIRE(result.error().code == Error::Code::NotSupported);
    }

    SECTION("onPropertyChanged callback updates engine volume status")
    {
      backendRaw->setMockPropertyInfo(PropertyId::Volume,
                                      PropertyInfo{
                                        .canRead = true,
                                        .canWrite = true,
                                        .isAvailable = true,
                                        .emitsChangeNotifications = false,
                                        .isHardwareAssisted = true,
                                      });

      // Play must be called so the backend target is initialized
      engine.play(desc);

      backendRaw->firePropertyChanged(PropertyId::Volume);

      REQUIRE(waitUntil([&] { return engine.status().volumeAvailable; }));
      REQUIRE(engine.status().volume == Catch::Approx{1.0F});
      REQUIRE(engine.volume() == Catch::Approx{1.0F});
      REQUIRE(engine.status().volumeAvailable == true);
      REQUIRE(engine.status().volumeIsHardwareAssisted == true);
    }

    SECTION("onPropertyChanged handles backend read errors gracefully")
    {
      backendRaw->setPropertyError(Error::Code::Generic);
      backendRaw->firePropertyChanged(PropertyId::Volume);
      backendRaw->firePropertyChanged(PropertyId::Muted);

      REQUIRE(waitUntil([&] { return engine.status().volumeAvailable; }));
      REQUIRE(engine.status().volume == Catch::Approx{1.0F});
      REQUIRE(engine.status().muted == false);
    }

    SECTION("onPropertyChanged callback updates engine mute status")
    {
      backendRaw->firePropertyChanged(PropertyId::Muted);

      REQUIRE(waitUntil([&] { return engine.status().volumeAvailable; }));
      REQUIRE(engine.status().muted == false);
      REQUIRE(engine.isMuted() == false);
    }

    SECTION("onPropertyChanged callback for unknown property is ignored")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      backendRaw->firePropertyChanged(kUnknownId);
    }

    SECTION("Backend callbacks update engine state correctly")
    {
      engine.play(desc);
      backendRaw->fireBackendError("hardware failed");
      REQUIRE(waitUntil([&] { return engine.status().transport == Transport::Error; }));

      engine.play(desc);
      auto routeChanged = std::atomic<bool>{false};
      engine.setOnRouteChanged([&](auto const&) { routeChanged.store(true, std::memory_order_release); });
      backendRaw->fireRouteReady("test-anchor");
      CHECK(waitUntil([&] { return routeChanged.load(std::memory_order_acquire); }));
    }

    SECTION("setVolume round-trips through engine and backend")
    {
      engine.setVolume(0.42F);
      REQUIRE(engine.volume() == Catch::Approx{0.42F});
      REQUIRE(engine.status().volume == Catch::Approx{0.42F});

      auto const backendVol = backendRaw->property(PropertyId::Volume);

      REQUIRE(backendVol);
      REQUIRE(std::get<float>(*backendVol) == Catch::Approx{0.42F});
    }

    SECTION("setMuted round-trips through engine and backend")
    {
      engine.setMuted(true);
      REQUIRE(engine.isMuted() == true);
      REQUIRE(engine.status().muted == true);

      auto const backendMuted = backendRaw->property(PropertyId::Muted);

      REQUIRE(backendMuted);
      REQUIRE(std::get<bool>(*backendMuted) == true);
    }

    SECTION("property controls survive backend open")
    {
      engine.setVolume(0.37F);
      engine.setMuted(true);

      engine.play(desc);

      CHECK(engine.status().volume == Catch::Approx{0.37F});
      CHECK(engine.status().muted == true);

      auto const backendVol = backendRaw->property(PropertyId::Volume);
      auto const backendMuted = backendRaw->property(PropertyId::Muted);

      REQUIRE(backendVol);
      REQUIRE(backendMuted);
      CHECK(std::get<float>(*backendVol) == Catch::Approx{0.37F});
      CHECK(std::get<bool>(*backendMuted) == true);
    }
  }

  TEST_CASE("Engine - Backend callback simulation", "[playback][unit][engine][callback]")
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
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};

    SECTION("Backend error transitions to Error state")
    {
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);

      auto* const target = backendRaw->target();

      target->onBackendError("Hardware failure");

      REQUIRE(waitUntil([&] { return engine.status().transport == Transport::Error; }));
      auto const snap = engine.status();

      REQUIRE(snap.transport == Transport::Error);
      REQUIRE(snap.statusText == "Hardware failure");
    }

    SECTION("Route ready updates snapshot")
    {
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);

      auto* const target = backendRaw->target();
      auto callbackThreadPromise = std::promise<std::thread::id>{};
      auto callbackThread = callbackThreadPromise.get_future();
      auto const callerThread = std::this_thread::get_id();

      engine.setOnStateChanged([&callbackThreadPromise]
                               { callbackThreadPromise.set_value(std::this_thread::get_id()); });

      target->onRouteReady("anchor-123");

      REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      CHECK(callbackThread.get() != callerThread);
      REQUIRE(waitUntil([&] { return engine.routeStatus().optAnchor.has_value(); }));
      auto const route = engine.routeStatus();

      REQUIRE(route.optAnchor);
      REQUIRE(route.optAnchor->id == "anchor-123");
    }

    SECTION("Playback status callbacks update engine internals")
    {
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);
      auto* const target = backendRaw->target();

      target->onUnderrun();

      target->onPositionAdvanced(100);

      target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});

      target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});

      target->onPropertyChanged(PropertyId::Volume);

      REQUIRE(waitUntil([&] { return engine.status().routeState.engineOutputFormat.sampleRate == 48000; }));
    }

    SECTION("close drops the retired render session target")
    {
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);
      auto* const target = backendRaw->target();
      REQUIRE(target != nullptr);

      engine.stop();

      auto const snap = engine.status();
      CHECK(snap.transport == Transport::Idle);
      CHECK(snap.statusText.empty());
      CHECK(snap.underrunCount == 0);
      CHECK_FALSE(engine.routeStatus().optAnchor);
      CHECK(backendRaw->target() == nullptr);
    }

    SECTION("User callbacks run outside backend callback stack and may reenter Engine")
    {
      auto const desc = PlaybackInput{.filePath = "song.flac"};

      engine.play(desc);

      auto callbackThreadPromise = std::promise<std::thread::id>{};
      auto callbackThread = callbackThreadPromise.get_future();
      auto const backendCallbackThread = std::this_thread::get_id();

      engine.setOnRouteChanged(
        [&](auto const&)
        {
          callbackThreadPromise.set_value(std::this_thread::get_id());
          engine.stop();
        });

      backendRaw->fireRouteReady("reentrant-anchor");

      REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      CHECK(callbackThread.get() != backendCallbackThread);
      REQUIRE(waitUntil([&] { return engine.status().transport == Transport::Idle; }));
    }

    SECTION("queued render event from retired session is ignored")
    {
      class BlockingStopBackend final : public IBackend
      {
      public:
        Result<> open(Format const& format, IRenderTarget* target) override
        {
          auto const lock = std::scoped_lock{_mutex};
          _format = format;
          _target = target;
          return {};
        }

        void start() override {}
        void pause() override {}
        void resume() override {}
        void flush() override {}

        void stop() override
        {
          auto lock = std::unique_lock{_mutex};

          if (!_blockStop)
          {
            return;
          }

          _stopEntered = true;
          _cv.notify_all();
          _cv.wait(lock, [this] { return _releaseStop; });
        }

        void close() override
        {
          auto const lock = std::scoped_lock{_mutex};
          _target = nullptr;
        }

        BackendId backendId() const noexcept override { return BackendId{"blocking-stop"}; }
        ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

        Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

        Result<PropertyValue> property(PropertyId id) const override
        {
          if (id == PropertyId::Volume)
          {
            return 1.0F;
          }

          if (id == PropertyId::Muted)
          {
            return false;
          }

          return std::unexpected(Error{.code = Error::Code::NotSupported});
        }

        PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
        {
          return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
        }

        void blockStop()
        {
          auto const lock = std::scoped_lock{_mutex};
          _blockStop = true;
        }

        bool waitForStopEntered(std::chrono::milliseconds timeout)
        {
          auto lock = std::unique_lock{_mutex};
          return _cv.wait_for(lock, timeout, [this] { return _stopEntered; });
        }

        void releaseStop()
        {
          auto const lock = std::scoped_lock{_mutex};
          _releaseStop = true;
          _cv.notify_all();
        }

        void fireRouteReady(std::string_view routeAnchor)
        {
          auto* target = static_cast<IRenderTarget*>(nullptr);
          {
            auto const lock = std::scoped_lock{_mutex};
            target = _target;
          }

          if (target != nullptr)
          {
            target->onRouteReady(routeAnchor);
          }
        }

      private:
        mutable std::mutex _mutex;
        std::condition_variable _cv;
        IRenderTarget* _target = nullptr;
        Format _format{};
        bool _blockStop = false;
        bool _stopEntered = false;
        bool _releaseStop = false;
      };

      auto blockingBackendPtr = std::make_unique<BlockingStopBackend>();
      auto* const blockingBackendRaw = blockingBackendPtr.get();
      auto blockingEngine = Engine{std::move(blockingBackendPtr), device, factory};

      auto routeChanged = std::atomic<bool>{false};
      blockingEngine.setOnRouteChanged([&](Engine::RouteStatus const&)
                                       { routeChanged.store(true, std::memory_order_release); });

      blockingEngine.play(PlaybackInput{.filePath = "song.flac"});
      REQUIRE(blockingEngine.status().transport == Transport::Playing);

      blockingBackendRaw->blockStop();
      auto stopFuture = std::async(std::launch::async, [&] { blockingEngine.stop(); });
      REQUIRE(blockingBackendRaw->waitForStopEntered(std::chrono::seconds{1}));

      blockingBackendRaw->fireRouteReady("stale-anchor");
      CHECK_FALSE(
        waitUntil([&] { return routeChanged.load(std::memory_order_acquire); }, std::chrono::milliseconds{50}));

      blockingBackendRaw->releaseStop();
      REQUIRE(stopFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

      CHECK_FALSE(
        waitUntil([&] { return routeChanged.load(std::memory_order_acquire); }, std::chrono::milliseconds{50}));
      CHECK_FALSE(blockingEngine.routeStatus().optAnchor);
      CHECK(blockingEngine.status().transport == Transport::Idle);
    }

    SECTION("setBackend with active track resumes playback")
    {
      auto const desc = PlaybackInput{.filePath = "test.flac"};
      engine.play(desc);
      REQUIRE(engine.status().transport == Transport::Playing);

      auto newBackendPtr = std::make_unique<CapturingBackend>();
      auto const newDevice = Device{.id = DeviceId{"new-device"},
                                    .displayName = "New",
                                    .description = "New",
                                    .isDefault = false,
                                    .backendId = kBackendNone};
      engine.setBackend(std::move(newBackendPtr), newDevice);

      auto const snap = engine.status();
      REQUIRE(snap.transport == Transport::Playing);
      REQUIRE(snap.currentDeviceId == "new-device");
    }

    SECTION("Engine::resume on already playing engine does nothing")
    {
      engine.play(PlaybackInput{.filePath = "test.flac"});
      REQUIRE(engine.status().transport == Transport::Playing);
      engine.resume();
      REQUIRE(engine.status().transport == Transport::Playing);
    }

    SECTION("Engine::pause on Idle engine does nothing")
    {
      engine.stop();
      engine.pause();
      REQUIRE(engine.status().transport == Transport::Idle);
    }
  }

  class BlockingPropertyBackend final : public IBackend
  {
  public:
    Result<> open(Format const& /*format*/, IRenderTarget* /*target*/) override { return {}; }
    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void stop() override {}
    void close() override {}

    BackendId backendId() const noexcept override { return BackendId{"blocking"}; }
    ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

    Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override
    {
      auto lock = std::unique_lock{_mutex};
      ++_enteredCalls;
      ++_activeCalls;
      _maxActiveCalls = std::max(_maxActiveCalls, _activeCalls);
      _cv.notify_all();

      _cv.wait(lock, [this] { return _releaseCalls; });
      --_activeCalls;
      _cv.notify_all();
      return {};
    }

    Result<PropertyValue> property(PropertyId id) const override
    {
      if (id == PropertyId::Volume)
      {
        return PropertyValue{1.0F};
      }

      if (id == PropertyId::Muted)
      {
        return PropertyValue{false};
      }

      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
    {
      return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
    }

    bool waitForEnteredCalls(std::size_t count, std::chrono::milliseconds timeout) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this, count] { return _enteredCalls >= count; });
    }

    void releaseCalls()
    {
      auto const lock = std::scoped_lock{_mutex};
      _releaseCalls = true;
      _cv.notify_all();
    }

    std::size_t maxActiveCalls() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _maxActiveCalls;
    }

  private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _cv;
    std::size_t _enteredCalls = 0;
    std::size_t _activeCalls = 0;
    std::size_t _maxActiveCalls = 0;
    bool _releaseCalls = false;
  };

  TEST_CASE("Engine - concurrent control commands are serialized", "[playback][unit][engine][concurrency]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<BlockingPropertyBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device};

    auto first = std::async(std::launch::async, [&engine] { engine.setVolume(0.25F); });
    auto const firstEntered = backendRaw->waitForEnteredCalls(1, std::chrono::seconds{1});

    if (!firstEntered)
    {
      backendRaw->releaseCalls();
    }

    REQUIRE(firstEntered);

    auto secondStartedPromise = std::promise<void>{};
    auto secondStarted = secondStartedPromise.get_future();
    auto second = std::async(std::launch::async,
                             [&]
                             {
                               secondStartedPromise.set_value();
                               engine.setMuted(true);
                             });

    auto const secondStartedStatus = secondStarted.wait_for(std::chrono::seconds{1});

    if (secondStartedStatus == std::future_status::ready)
    {
      CHECK_FALSE(backendRaw->waitForEnteredCalls(2, std::chrono::milliseconds{50}));
    }

    backendRaw->releaseCalls();

    REQUIRE(secondStartedStatus == std::future_status::ready);
    REQUIRE(first.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(second.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(backendRaw->maxActiveCalls() == 1);
  }

  TEST_CASE("Engine - Source Error Propagation", "[playback][unit][engine][error]")
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
    REQUIRE(waitUntil([&] { return errorFuture.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready; },
                      std::chrono::seconds{15}));

    auto const snap = engine.status();
    REQUIRE(snap.transport == Transport::Error);
    REQUIRE(snap.statusText == "decode failed");
  }

  // A backend that faithfully models a real render thread: start() spawns a
  // thread that hammers the lock-free readPcm/isSourceDrained path, stop() joins
  // it. This mirrors the Engine's quiescent-point contract — sources are retired
  // (publishSource) only after backendPtr->stop() has joined the render thread —
  // so the simulated render thread never dereferences a freed source.
  class RenderingBackend final : public IBackend
  {
  public:
    Result<> open(Format const& format, IRenderTarget* target) override
    {
      _format = format;
      _target.store(target, std::memory_order_relaxed);
      return {};
    }

    void start() override
    {
      if (_thread.joinable())
      {
        return;
      }

      _thread = std::jthread{[this](std::stop_token const& st)
                             {
                               auto buffer = std::array<std::byte, 1024>{};

                               while (!st.stop_requested())
                               {
                                 if (auto* const t = _target.load(std::memory_order_relaxed); t != nullptr)
                                 {
                                   std::ignore = t->readPcm(buffer);
                                   std::ignore = t->isSourceDrained();
                                 }
                               }
                             }};
    }

    void pause() override {}
    void resume() override {}
    void flush() override {}

    void stop() override
    {
      _thread.request_stop();

      if (_thread.joinable())
      {
        _thread.join();
      }
    }

    void close() override {}

    BackendId backendId() const noexcept override { return BackendId{"rendering"}; }
    ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

    Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

    Result<PropertyValue> property(PropertyId id) const override
    {
      if (id == PropertyId::Volume)
      {
        return PropertyValue{1.0F};
      }

      if (id == PropertyId::Muted)
      {
        return PropertyValue{false};
      }

      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
    {
      return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
    }

  private:
    std::atomic<IRenderTarget*> _target{nullptr};
    Format _format{};
    std::jthread _thread;
  };

  // Run under TSan (./ao test --tsan): the control thread loops play/seek/stop
  // (each publishing and retiring a source) while a render thread reads the
  // lock-free RenderSourceSlot pointer and a poller reads status() through the
  // source slot owner. TSan verifies the publish/retire happens-before chain
  // holds.
  TEST_CASE("Engine - concurrent source swap is race-free", "[playback][unit][engine][concurrency]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(4096, std::byte{0});
      decPtr->setReadScript({{data, false}, {data, false}, {data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::make_unique<RenderingBackend>(), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    // Poller: read status() concurrently with the control thread's source swaps.
    auto poller = std::jthread{[&](std::stop_token const& st)
                               {
                                 while (!st.stop_requested())
                                 {
                                   std::ignore = engine.status();
                                 }
                               }};

    for (std::int32_t i = 0; i < 50; ++i)
    {
      engine.play(desc);
      engine.seek(std::chrono::milliseconds{10});
      engine.stop();
    }

    poller.request_stop();

    if (poller.joinable())
    {
      poller.join();
    }

    REQUIRE(engine.status().transport == Transport::Idle);
  }
} // namespace ao::audio::test
