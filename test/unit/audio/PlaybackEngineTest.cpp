#include "CapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "fakeit.hpp"

#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/utility/IMainThreadDispatcher.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/Error.h>

using namespace ao::audio;
using namespace fakeit;

namespace
{
  class MockDispatcher : public ao::IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };

  class MockBackendProxy final : public IBackend
  {
    IBackend& _real;

  public:
    MockBackendProxy(IBackend& real)
      : _real(real)
    {
    }
    ao::Result<> open(Format const& f, RenderCallbacks c) override { return _real.open(f, c); }
    void reset() override { _real.reset(); }
    void start() override { _real.start(); }
    void pause() override { _real.pause(); }
    void resume() override { _real.resume(); }
    void flush() override { _real.flush(); }
    void drain() override { _real.drain(); }
    void stop() override { _real.stop(); }
    void close() override { _real.close(); }
    void setExclusiveMode(bool e) override { _real.setExclusiveMode(e); }
    bool isExclusiveMode() const noexcept override { return _real.isExclusiveMode(); }
    BackendId backendId() const noexcept override { return _real.backendId(); }
    ProfileId profileId() const noexcept override { return _real.profileId(); }
    void setVolume(float v) override { _real.setVolume(v); }
    float getVolume() const override { return _real.getVolume(); }
    void setMuted(bool m) override { _real.setMuted(m); }
    bool isMuted() const override { return _real.isMuted(); }
    bool isVolumeAvailable() const override { return _real.isVolumeAvailable(); }
  };
}

TEST_CASE("Engine - Basic Orchestration", "[playback][engine]")
{
  Mock<IBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};

  Fake(Method(mockBackend, open));
  Fake(Method(mockBackend, reset));
  Fake(Method(mockBackend, start));
  Fake(Method(mockBackend, pause));
  Fake(Method(mockBackend, resume));
  Fake(Method(mockBackend, flush));
  Fake(Method(mockBackend, drain));
  Fake(Method(mockBackend, stop));
  Fake(Method(mockBackend, close));
  Fake(Method(mockBackend, setExclusiveMode));
  When(Method(mockBackend, isExclusiveMode)).AlwaysReturn(false);
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendNone);
  When(Method(mockBackend, profileId)).AlwaysReturn(kProfileShared);
  Fake(Method(mockBackend, setVolume), Method(mockBackend, setMuted));
  When(Method(mockBackend, getVolume)).AlwaysReturn(1.0F);
  When(Method(mockBackend, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend, isVolumeAvailable)).AlwaysReturn(true);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());

  auto engine = Engine{std::move(backendPtr), device, dispatcher};

  SECTION("Stop correctly cleans up backend")
  {
    engine.stop();
    Verify(Method(mockBackend, reset)).AtLeastOnce();
    Verify(Method(mockBackend, stop)).AtLeastOnce();
    Verify(Method(mockBackend, close)).AtLeastOnce();

    auto snap = engine.status();
    REQUIRE(snap.transport == Transport::Idle);
  }

  SECTION("Backend error transitions to Error state")
  {
    Engine::onBackendError(&engine, "Hardware failure");

    auto snap = engine.status();
    REQUIRE(snap.transport == Transport::Error);
    REQUIRE(snap.statusText == "Hardware failure");
  }

  SECTION("Route ready updates snapshot")
  {
    Engine::onRouteReady(&engine, "anchor-123");

    auto route = engine.routeStatus();
    REQUIRE(route.optAnchor);
    REQUIRE(route.optAnchor->id == "anchor-123");
  }

  SECTION("Volume and mute controls pass through to backend and update status")
  {
    engine.setVolume(0.75F);
    Verify(Method(mockBackend, setVolume).Using(0.75F)).Once();
    REQUIRE(engine.getVolume() == Catch::Approx(0.75F));
    REQUIRE(engine.status().volume == Catch::Approx(0.75F));

    engine.setMuted(true);
    Verify(Method(mockBackend, setMuted).Using(true)).Once();
    REQUIRE(engine.isMuted() == true);
    REQUIRE(engine.status().muted == true);

    REQUIRE(engine.isVolumeAvailable() == true);
    REQUIRE(engine.status().volumeAvailable == true);
  }
}

TEST_CASE("Engine - Backend Swapping", "[playback][engine][hot-swap]")
{
  Mock<IBackend> mockBackend1;
  Mock<IBackend> mockBackend2;
  auto dispatcher = std::make_shared<MockDispatcher>();

  Fake(
    Method(mockBackend1, open), Method(mockBackend1, reset), Method(mockBackend1, stop), Method(mockBackend1, close));
  Fake(
    Method(mockBackend2, open), Method(mockBackend2, reset), Method(mockBackend2, stop), Method(mockBackend2, close));

  When(Method(mockBackend1, backendId)).AlwaysReturn(kBackendNone);
  When(Method(mockBackend1, profileId)).AlwaysReturn(kProfileShared);

  When(Method(mockBackend2, backendId)).AlwaysReturn(kBackendAlsa);
  When(Method(mockBackend2, profileId)).AlwaysReturn(kProfileExclusive);

  Fake(Method(mockBackend1, setVolume), Method(mockBackend1, setMuted));
  When(Method(mockBackend1, getVolume)).AlwaysReturn(1.0F);
  When(Method(mockBackend1, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend1, isVolumeAvailable)).AlwaysReturn(true);

  Fake(Method(mockBackend2, setVolume), Method(mockBackend2, setMuted));
  When(Method(mockBackend2, getVolume)).AlwaysReturn(1.0F);
  When(Method(mockBackend2, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend2, isVolumeAvailable)).AlwaysReturn(true);

  auto backend1 = std::make_unique<MockBackendProxy>(mockBackend1.get());
  auto engine = Engine{
    std::move(backend1),
    {.id = DeviceId{"dev1"}, .displayName = "D1", .description = "D1", .isDefault = false, .backendId = kBackendNone},
    dispatcher};

  SECTION("Switching backend while idle")
  {
    auto backend2 = std::make_unique<MockBackendProxy>(mockBackend2.get());
    engine.setBackend(std::move(backend2),
                      {.id = DeviceId{"dev2"},
                       .displayName = "D2",
                       .description = "D2",
                       .isDefault = false,
                       .backendId = kBackendAlsa});

    Verify(Method(mockBackend1, reset)).Once();
    Verify(Method(mockBackend1, stop)).Once();
    Verify(Method(mockBackend1, close)).Once();

    auto snap = engine.status();
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

  Mock<IBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};

  Fake(Method(mockBackend, open));
  Fake(Method(mockBackend, reset));
  Fake(Method(mockBackend, start));
  Fake(Method(mockBackend, pause));
  Fake(Method(mockBackend, resume));
  Fake(Method(mockBackend, flush));
  Fake(Method(mockBackend, drain));
  Fake(Method(mockBackend, stop));
  Fake(Method(mockBackend, close));
  Fake(Method(mockBackend, setExclusiveMode));
  When(Method(mockBackend, isExclusiveMode)).AlwaysReturn(false);
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendNone);
  When(Method(mockBackend, profileId)).AlwaysReturn(kProfileShared);
  Fake(Method(mockBackend, setVolume), Method(mockBackend, setMuted));
  When(Method(mockBackend, getVolume)).AlwaysReturn(1.0F);
  When(Method(mockBackend, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend, isVolumeAvailable)).AlwaysReturn(true);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  auto engine = Engine{std::move(backendPtr), device, dispatcher};

  auto const descriptor =
    TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Test Title", .artist = "Test Artist"};

  engine.play(descriptor);

  auto snap = engine.status();

  SECTION("rs-decoder is present in the graph")
  {
    auto it = std::ranges::find(snap.flow.nodes, "rs-decoder", &ao::audio::flow::Node::id);
    REQUIRE(it != snap.flow.nodes.end());
    CHECK(it->type == flow::NodeType::Decoder);
    CHECK(it->optFormat);
    CHECK(it->optFormat->sampleRate == 44100);
  }

  SECTION("rs-engine is present in the graph")
  {
    auto it = std::ranges::find(snap.flow.nodes, "rs-engine", &ao::audio::flow::Node::id);
    REQUIRE(it != snap.flow.nodes.end());
    CHECK(it->type == flow::NodeType::Engine);
  }

  SECTION("rs-decoder is linked to rs-engine")
  {
    auto it = std::ranges::find_if(snap.flow.connections,
                                   [](auto const& connection)
                                   { return connection.sourceId == "rs-decoder" && connection.destId == "rs-engine"; });
    CHECK(it != snap.flow.connections.end());
    if (it != snap.flow.connections.end())
    {
      CHECK(it->isActive);
    }
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

  Mock<IBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"pipewire-shared"},
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
        return ao::Result<>();
      });
  Fake(Method(mockBackend, reset));
  Fake(Method(mockBackend, start));
  Fake(Method(mockBackend, pause));
  Fake(Method(mockBackend, resume));
  Fake(Method(mockBackend, flush));
  Fake(Method(mockBackend, drain));
  Fake(Method(mockBackend, stop));
  Fake(Method(mockBackend, close));
  Fake(Method(mockBackend, setExclusiveMode));
  When(Method(mockBackend, isExclusiveMode)).AlwaysReturn(false);
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendPipeWire);
  When(Method(mockBackend, profileId)).AlwaysReturn(kProfileShared);

  Fake(Method(mockBackend, setVolume));
  When(Method(mockBackend, getVolume)).AlwaysReturn(1.0F);
  Fake(Method(mockBackend, setMuted));
  When(Method(mockBackend, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend, isVolumeAvailable)).AlwaysReturn(true);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  auto engine = Engine{std::move(backendPtr), device, dispatcher};

  auto const descriptor =
    TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "PipeWire Shared", .artist = "Test Artist"};

  engine.play(descriptor);

  Verify(Method(mockBackend, reset)).AtLeastOnce();
  REQUIRE(openedFormats.size() >= 1);
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

  Mock<IBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"alsa-exclusive"},
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
        return ao::Result<>();
      });
  Fake(Method(mockBackend, reset));
  Fake(Method(mockBackend, start));
  Fake(Method(mockBackend, pause));
  Fake(Method(mockBackend, resume));
  Fake(Method(mockBackend, flush));
  Fake(Method(mockBackend, drain));
  Fake(Method(mockBackend, stop));
  Fake(Method(mockBackend, close));
  Fake(Method(mockBackend, setExclusiveMode));
  When(Method(mockBackend, isExclusiveMode)).AlwaysReturn(true);
  When(Method(mockBackend, backendId)).AlwaysReturn(kBackendAlsa);
  When(Method(mockBackend, profileId)).AlwaysReturn(kProfileExclusive);

  Fake(Method(mockBackend, setVolume));
  When(Method(mockBackend, getVolume)).AlwaysReturn(1.0F);
  Fake(Method(mockBackend, setMuted));
  When(Method(mockBackend, isMuted)).AlwaysReturn(false);
  When(Method(mockBackend, isVolumeAvailable)).AlwaysReturn(true);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  auto engine = Engine{std::move(backendPtr), device, dispatcher};

  auto const descriptor =
    TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Unsupported Sample Rate", .artist = "Test Artist"};

  engine.play(descriptor);

  Verify(Method(mockBackend, reset)).AtLeastOnce();
  auto const snap = engine.status();
  REQUIRE(snap.transport == Transport::Error);
  CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
  REQUIRE(openedFormats.size() == 0);
}

TEST_CASE("Engine - Play failure matrix", "[playback][engine][error]")
{
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};

  SECTION("Unsupported extension")
  {
    auto engine = Engine{std::make_unique<CapturingBackend>(), device, dispatcher};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.txt", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};
    engine.play(desc);

    REQUIRE(engine.status().transport == Transport::Error);
    REQUIRE(engine.status().statusText.find("decoder") != std::string::npos);
  }

  SECTION("Decoder open failure")
  {
    auto factory = [](auto const&, auto const& fmt)
    {
      auto dec = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
      dec->setOpenResult(std::unexpected(ao::Error{.message = "open failed"}));
      return dec;
    };

    auto engine = Engine{std::make_unique<CapturingBackend>(), device, dispatcher, factory};
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

    auto factory = [](auto const&, auto const& fmt)
    {
      auto dec = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt,
        .outputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true},
        .durationMs = 0,
        .isLossy = false});
      dec->setReadScript({{{}, true}});
      return dec;
    };

    auto engine = Engine{std::move(backend), device, dispatcher, factory};
    auto const desc = TrackPlaybackDescriptor{
      .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};
    engine.play(desc);

    REQUIRE(engine.status().transport == Transport::Error);
    REQUIRE(engine.status().statusText == "hw init failed");
  }
}

TEST_CASE("Engine - Pause and resume matrix", "[playback][engine][transport]")
{
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();
  auto* backendPtr = backend.get();

  Format fmt{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  auto factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    // provide some data for preroll
    std::vector<std::byte> data(100, std::byte{0});
    dec->setReadScript({{data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, dispatcher, factory};
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
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();

  Format fmt{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true}; // 2 bytes = 1ms
  auto factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    std::vector<std::byte> data(200, std::byte{0}); // 100ms
    dec->setReadScript({{data, false}, {data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, dispatcher, factory};
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
  auto dispatcher = std::make_shared<MockDispatcher>();
  Device device{.id = DeviceId{"test-device"},
                .displayName = "Test",
                .description = "Test",
                .isDefault = false,
                .backendId = kBackendNone};
  auto backend = std::make_unique<CapturingBackend>();

  Format fmt{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
  auto factory = [fmt](auto const&, auto const&)
  {
    auto dec = std::make_unique<ScriptedDecoderSession>(
      DecodedStreamInfo{.sourceFormat = fmt, .outputFormat = fmt, .durationMs = 0, .isLossy = false});
    std::vector<std::byte> data(20, std::byte{0}); // 10ms
    dec->setReadScript({{data, false}, {{}, true}});
    return dec;
  };

  auto engine = Engine{std::move(backend), device, dispatcher, factory};
  auto const desc = TrackPlaybackDescriptor{
    .filePath = "song.flac", .title = "Test", .artist = "Test", .album = "Test", .optCoverArtId = std::nullopt};

  bool trackEnded = false;
  engine.setOnTrackEnded([&]() { trackEnded = true; });

  engine.play(desc);

  // Simulate playback loop
  auto buffer = std::array<std::byte, 100>{};
  Engine::onReadPcm(&engine, buffer); // Read all 20 bytes

  REQUIRE(Engine::isSourceDrained(&engine));

  SECTION("onDrainComplete resets to idle and fires track ended")
  {
    Engine::onDrainComplete(&engine);
    REQUIRE(engine.status().transport == Transport::Idle);
    REQUIRE(trackEnded == true);
  }

  SECTION("onDrainComplete without pending drain is ignored")
  {
    engine.stop(); // resets everything
    trackEnded = false;
    Engine::onDrainComplete(&engine);
    REQUIRE(trackEnded == false);
  }
}
