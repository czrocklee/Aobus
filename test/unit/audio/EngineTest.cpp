// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "TestUtility.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/Types.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
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

    auto const descriptor =
      TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Test Title", .artist = "Test Artist"};

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

    auto const descriptor =
      TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "PipeWire Shared", .artist = "Test Artist"};

    engine.play(descriptor);

    REQUIRE(!openedFormats.empty());
    CHECK(openedFormats.back().sampleRate == 44100);
    CHECK(openedFormats.back().channels == 2);
    CHECK(openedFormats.back().bitDepth == 16);
    CHECK(engine.status().transport == Transport::Playing);

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

    auto const descriptor = TrackPlaybackDescriptor{
      .filePath = testFile.string(), .title = "Unsupported Sample Rate", .artist = "Test Artist"};

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
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.txt", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

      engine.play(desc);

      REQUIRE(engine.status().transport == Transport::Error);
      REQUIRE(engine.status().statusText.find("decoder") != std::string::npos);
    }

    SECTION("Decoder open failure")
    {
      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(
          DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});

        decPtr->setOpenResult(std::unexpected(Error{.message = "open failed"}));
        return decPtr;
      };

      auto engine = Engine{std::make_unique<CapturingBackend>(), device, factory};
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

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
          .durationMs = 0,
          .isLossy = false});

        decPtr->setReadScript({{{}, true}});
        return decPtr;
      };

      auto engine = Engine{std::move(backendPtr), device, factory};
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});

      // provide some data for preroll
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
      auto data = std::vector(200, std::byte{0}); // 100ms

      decPtr->setReadScript({{data, false}, {data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

    SECTION("Seek before play is no-op")
    {
      engine.seek(100);
      REQUIRE(engine.status().positionMs == 0);
    }

    SECTION("Active seek success")
    {
      engine.play(desc);
      engine.seek(50);
      REQUIRE(engine.status().positionMs == 50);
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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
      auto data = std::vector(20, std::byte{0}); // 10ms

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

    bool trackEnded = false;
    engine.setOnTrackEnded([&] { trackEnded = true; });

    engine.play(desc);

    // Simulate playback loop via backend callbacks
    auto* const target = backendRaw->target();
    auto buffer = std::array<std::byte, 100>{};

    target->readPcm(buffer); // Read all 20 bytes

    REQUIRE(target->isSourceDrained());

    SECTION("onDrainComplete resets to idle and fires track ended")
    {
      backendRaw->fireDrainComplete();
      REQUIRE(engine.status().transport == Transport::Idle);
      REQUIRE(trackEnded == true);
    }

    SECTION("onDrainComplete without pending drain is ignored")
    {
      engine.stop(); // resets everything
      trackEnded = false;
      backendRaw->fireDrainComplete();
      REQUIRE(trackEnded == false);
    }

    SECTION("onBackendError stops playback")
    {
      backendRaw->fireBackendError("lost device");
      REQUIRE(engine.status().transport == Transport::Error);
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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 1000, .isLossy = false});
      decPtr->setReadScript({{.data = std::vector<std::byte>(88200, std::byte{0}), .endOfStream = false}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = TrackPlaybackDescriptor{.filePath = "test.flac", .title = "Test"};

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

      // wait for the async task to run
      std::this_thread::sleep_for(std::chrono::milliseconds{200});

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

      REQUIRE(engine.status().volume == Catch::Approx{1.0F});
      REQUIRE(engine.status().muted == false);
    }

    SECTION("onPropertyChanged callback updates engine mute status")
    {
      backendRaw->firePropertyChanged(PropertyId::Muted);

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
      CHECK(engine.status().transport == Transport::Error);

      engine.play(desc);
      bool routeChanged = false;
      engine.setOnRouteChanged([&](auto const&) { routeChanged = true; });
      backendRaw->fireRouteReady("test-anchor");
      CHECK(routeChanged == true);
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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};

    SECTION("Backend error transitions to Error state")
    {
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.flac", .title = "T", .artist = "A", .album = "", .optCoverArtId = std::nullopt};

      engine.play(desc);

      auto* const target = backendRaw->target();

      target->onBackendError("Hardware failure");

      auto const snap = engine.status();

      REQUIRE(snap.transport == Transport::Error);
      REQUIRE(snap.statusText == "Hardware failure");
    }

    SECTION("Route ready updates snapshot")
    {
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.flac", .title = "T", .artist = "A", .album = "", .optCoverArtId = std::nullopt};

      engine.play(desc);

      auto* const target = backendRaw->target();

      target->onRouteReady("anchor-123");

      auto const route = engine.routeStatus();

      REQUIRE(route.optAnchor);
      REQUIRE(route.optAnchor->id == "anchor-123");
    }

    SECTION("Playback status callbacks update engine internals")
    {
      auto const desc = TrackPlaybackDescriptor{
        .filePath = "song.flac", .title = "T", .artist = "A", .album = "", .optCoverArtId = std::nullopt};

      engine.play(desc);
      auto* const target = backendRaw->target();

      target->onUnderrun();

      target->onPositionAdvanced(100);

      target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});

      target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});

      target->onPropertyChanged(PropertyId::Volume);
    }

    SECTION("setBackend with active track resumes playback")
    {
      auto const desc = TrackPlaybackDescriptor{.filePath = "test.flac", .title = "Test"};
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
      engine.play(TrackPlaybackDescriptor{.filePath = "test.flac"});
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
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 1000, .isLossy = false});

      // First block succeeds (preroll), second block fails
      // 100,000 bytes at 44.1kHz stereo 16-bit is ~566ms, satisfying the 500ms preroll
      decPtr->setReadScript(
        {{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false},
         {.data = {}, .endOfStream = false, .result = std::unexpected(Error{.message = "decode failed"})}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "fail.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

    auto errorPromise = std::promise<void>{};
    auto errorFuture = errorPromise.get_future();
    engine.setOnTrackEnded([&errorPromise] { errorPromise.set_value(); });

    engine.play(desc);

    // The StreamingSource decode loop runs in a background thread.
    // It should hit the error and call handleSourceError, which now
    // fires onTrackEnded so we can synchronize without polling.
    auto const status = errorFuture.wait_for(std::chrono::seconds{5});

    REQUIRE(status == std::future_status::ready);

    auto const snap = engine.status();
    REQUIRE(snap.transport == Transport::Error);
    REQUIRE(snap.statusText == "decode failed");
  }
} // namespace ao::audio::test
