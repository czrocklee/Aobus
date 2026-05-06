#include "CapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "TestUtility.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

using namespace ao::audio;
using namespace ao::audio::test;
using namespace fakeit;

TEST_CASE("Engine - Basic Orchestration", "[playback][engine]")
{
  auto spy = SpyBackend<>{};
  auto& mockBackend = spy.mock();
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};

  auto engine = Engine{spy.make_proxy(), device};

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
    PropertyId lastSetPropertyId{};
    auto lastSetPropertyValue = PropertyValue{false};

    When(Method(mockBackend, setProperty))
      .AlwaysDo(
        [&](PropertyId id, PropertyValue const& value) -> ao::Result<>
        {
          lastSetPropertyId = id;
          lastSetPropertyValue = value;
          return ao::Result<>{};
        });

    engine.setVolume(0.75F);
    REQUIRE(lastSetPropertyId == PropertyId::Volume);
    REQUIRE(std::get<float>(lastSetPropertyValue) == Catch::Approx(0.75F));
    REQUIRE(engine.getVolume() == Catch::Approx(0.75F));
    REQUIRE(engine.status().volume == Catch::Approx(0.75F));

    engine.setMuted(true);
    REQUIRE(lastSetPropertyId == PropertyId::Muted);
    REQUIRE(std::get<bool>(lastSetPropertyValue) == true);
    REQUIRE(engine.isMuted() == true);
    REQUIRE(engine.status().muted == true);

    REQUIRE(engine.isVolumeAvailable() == true);
    REQUIRE(engine.status().volumeAvailable == true);
  }
}

TEST_CASE("Engine - Backend Swapping", "[playback][engine][hot-swap]")
{
  auto spy1 = SpyBackend<>{};
  auto spy2 = SpyBackend<>{};
  auto& mockBackend1 = spy1.mock();
  auto& mockBackend2 = spy2.mock();

  When(Method(mockBackend1, backendId)).AlwaysReturn(kBackendNone);
  When(Method(mockBackend1, profileId)).AlwaysReturn(kProfileShared);

  When(Method(mockBackend2, backendId)).AlwaysReturn(kBackendAlsa);
  When(Method(mockBackend2, profileId)).AlwaysReturn(kProfileExclusive);

  auto engine = Engine{
    spy1.make_proxy(),
    {.id = DeviceId{"dev1"}, .displayName = "D1", .description = "D1", .isDefault = false, .backendId = kBackendNone}};

  SECTION("Switching backend while idle")
  {
    engine.setBackend(spy2.make_proxy(),
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

TEST_CASE("Engine - Graph Initialization", "[playback][engine][graph]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping Graph Integrity test");
    return;
  }

  auto spy = SpyBackend<>{};
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};

  auto engine = Engine{spy.make_proxy(), device};

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

TEST_CASE("Engine - PipeWire shared mode keeps native sample rate", "[playback][engine][pipewire]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

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
      [&](Format const& format, RenderCallbacks)
      {
        openedFormats.push_back(format);
        return ao::Result<>{};
      });
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendPipeWire);

  auto engine = Engine{spy.make_proxy(), device};

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

TEST_CASE("Engine - Unsupported backend sample rate fails without resampler", "[playback][engine][format]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

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
      [&](Format const& format, RenderCallbacks)
      {
        openedFormats.push_back(format);
        return ao::Result<>{};
      });
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendAlsa);
  When(Method(mockBackend, profileId)).AlwaysReturn(kProfileExclusive);

  auto engine = Engine{spy.make_proxy(), device};

  auto const descriptor =
    TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Unsupported Sample Rate", .artist = "Test Artist"};

  engine.play(descriptor);

  auto const snap = engine.status();
  REQUIRE(snap.transport == Transport::Error);
  CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
  REQUIRE(openedFormats.empty());
}

TEST_CASE("Engine - Play failure matrix", "[playback][engine][error]")
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
      auto dec = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});

      dec->setOpenResult(std::unexpected(ao::Error{.message = "open failed"}));
      return dec;
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
    auto backend = std::make_unique<CapturingBackend>();

    backend->setOpenResult(std::unexpected(ao::Error{.message = "hw init failed"}));

    auto const factory = [](auto const&, auto const& fmt)
    {
      auto dec = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt,
        .outputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true},
        .durationMs = 0,
        .isLossy = false});

      dec->setReadScript({{{}, true}});
      return dec;
    };

    auto engine = Engine{std::move(backend), device, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

    engine.play(desc);

    REQUIRE(engine.status().transport == Transport::Error);
    REQUIRE(engine.status().statusText == "hw init failed");
  }
}

TEST_CASE("Engine - Pause and resume matrix", "[playback][engine][transport]")
{
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();
  auto* const backendPtr = backend.get();

  auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  auto const factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});

    // provide some data for preroll
    std::vector<std::byte> data(100, std::byte{0});

    dec->setReadScript({{data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, factory};
  auto const desc = TrackPlaybackDescriptor{
    .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

  engine.play(desc);
  REQUIRE(engine.status().transport == Transport::Playing);

  SECTION("Pause from Playing")
  {
    engine.pause();
    REQUIRE(engine.status().transport == Transport::Paused);
    REQUIRE(backendPtr->events().back().name == "pause");
  }

  SECTION("Resume from Paused")
  {
    engine.pause();
    backendPtr->clearEvents();
    engine.resume();
    REQUIRE(engine.status().transport == Transport::Playing);
    REQUIRE(backendPtr->events().back().name == "resume");
  }
}

TEST_CASE("Engine - Seek matrix", "[playback][engine][seek]")
{
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();

  auto const fmt = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true}; // 2 bytes = 1ms
  auto const factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    std::vector<std::byte> data(200, std::byte{0}); // 100ms

    dec->setReadScript({{data, false}, {data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, factory};
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

TEST_CASE("Engine - Drain and callback matrix", "[playback][engine][drain]")
{
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();
  auto* const backendPtr = backend.get();

  auto const fmt = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
  auto const factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    std::vector<std::byte> data(20, std::byte{0}); // 10ms

    dec->setReadScript({{data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, factory};
  auto const desc = TrackPlaybackDescriptor{
    .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

  bool trackEnded = false;
  auto const sub = engine.onTrackEnded([&]() { trackEnded = true; });

  engine.play(desc);

  // Simulate playback loop via backend callbacks
  auto const& cb = backendPtr->callbacks();
  auto buffer = std::array<std::byte, 100>{};

  cb.readPcm(cb.userData, buffer); // Read all 20 bytes

  REQUIRE(cb.isSourceDrained(cb.userData));

  SECTION("onDrainComplete resets to idle and fires track ended")
  {
    cb.onDrainComplete(cb.userData);
    REQUIRE(engine.status().transport == Transport::Idle);
    REQUIRE(trackEnded == true);
  }

  SECTION("onDrainComplete without pending drain is ignored")
  {
    engine.stop(); // resets everything
    trackEnded = false;
    cb.onDrainComplete(cb.userData);
    REQUIRE(trackEnded == false);
  }
}

TEST_CASE("Engine - Property API", "[playback][engine][property]")
{
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();
  auto* const backendPtr = backend.get();

  auto engine = Engine{std::move(backend), device};

  SECTION("queryProperty returns all-false for unknown PropertyId")
  {
    // Cast to an out-of-range value to simulate an unknown property
    auto constexpr kUnknownId = static_cast<PropertyId>(999);
    auto const info = backendPtr->queryProperty(kUnknownId);

    REQUIRE(info.canRead == false);
    REQUIRE(info.canWrite == false);
    REQUIRE(info.isAvailable == false);
    REQUIRE(info.emitsChangeNotifications == false);
  }

  SECTION("queryProperty returns valid info for Volume")
  {
    auto const info = backendPtr->queryProperty(PropertyId::Volume);

    REQUIRE(info.canRead == true);
    REQUIRE(info.canWrite == true);
    REQUIRE(info.isAvailable == true);
  }

  SECTION("setProperty returns error for unknown PropertyId")
  {
    auto constexpr kUnknownId = static_cast<PropertyId>(999);
    auto const result = backendPtr->setProperty(kUnknownId, PropertyValue{0.5F});

    REQUIRE(!result);
    REQUIRE(result.error().code == ao::Error::Code::NotSupported);
  }

  SECTION("getProperty returns error for unknown PropertyId")
  {
    auto constexpr kUnknownId = static_cast<PropertyId>(999);
    auto const result = backendPtr->getProperty(kUnknownId);

    REQUIRE(!result);
    REQUIRE(result.error().code == ao::Error::Code::NotSupported);
  }

  SECTION("onPropertyChanged callback updates engine volume status")
  {
    backendPtr->firePropertyChanged(PropertyId::Volume);

    REQUIRE(engine.status().volume == Catch::Approx(1.0F));
    REQUIRE(engine.getVolume() == Catch::Approx(1.0F));
    REQUIRE(engine.status().volumeAvailable == true);
  }

  SECTION("onPropertyChanged callback updates engine mute status")
  {
    backendPtr->firePropertyChanged(PropertyId::Muted);

    REQUIRE(engine.status().muted == false);
    REQUIRE(engine.isMuted() == false);
  }

  SECTION("setVolume round-trips through engine and backend")
  {
    engine.setVolume(0.42F);
    REQUIRE(engine.getVolume() == Catch::Approx(0.42F));
    REQUIRE(engine.status().volume == Catch::Approx(0.42F));

    auto const backendVol = backendPtr->getProperty(PropertyId::Volume);

    REQUIRE(backendVol);
    REQUIRE(std::get<float>(*backendVol) == Catch::Approx(0.42F));
  }

  SECTION("setMuted round-trips through engine and backend")
  {
    engine.setMuted(true);
    REQUIRE(engine.isMuted() == true);
    REQUIRE(engine.status().muted == true);

    auto const backendMuted = backendPtr->getProperty(PropertyId::Muted);

    REQUIRE(backendMuted);
    REQUIRE(std::get<bool>(*backendMuted) == true);
  }
}

TEST_CASE("Engine - Backend callback simulation", "[playback][engine][callback]")
{
  auto const device = Device{.id = DeviceId{"test-device"},
                             .displayName = "Test",
                             .description = "Test",
                             .isDefault = false,
                             .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();
  auto* const backendPtr = backend.get();

  auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  auto const factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    std::vector<std::byte> data(100, std::byte{0});

    dec->setReadScript({{data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, factory};

  SECTION("Backend error transitions to Error state")
  {
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "T", .artist = "A", .album = "", .optCoverArtId = std::nullopt};

    engine.play(desc);

    auto const& cb = backendPtr->callbacks();

    cb.onBackendError(cb.userData, "Hardware failure");

    auto const snap = engine.status();

    REQUIRE(snap.transport == Transport::Error);
    REQUIRE(snap.statusText == "Hardware failure");
  }

  SECTION("Route ready updates snapshot")
  {
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "T", .artist = "A", .album = "", .optCoverArtId = std::nullopt};

    engine.play(desc);

    auto const& cb = backendPtr->callbacks();

    cb.onRouteReady(cb.userData, "anchor-123");

    auto const route = engine.routeStatus();

    REQUIRE(route.optAnchor);
    REQUIRE(route.optAnchor->id == "anchor-123");
  }
}
