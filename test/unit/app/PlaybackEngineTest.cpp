#include "fakeit.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <rs/audio/IAudioBackend.h>
#include <rs/audio/PlaybackEngine.h>
#include <rs/utility/IMainThreadDispatcher.h>

#include <rs/Error.h>

using namespace rs::audio;
using namespace rs::audio;
using namespace rs::audio;
using namespace fakeit;

namespace
{
  class MockDispatcher : public rs::IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };

  class MockBackendProxy : public IAudioBackend
  {
    IAudioBackend& _real;

  public:
    MockBackendProxy(IAudioBackend& real)
      : _real(real)
    {
    }
    rs::Result<> open(AudioFormat const& f, AudioRenderCallbacks c) override { return _real.open(f, c); }
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
    BackendKind kind() const noexcept override { return _real.kind(); }
  };
}

TEST_CASE("PlaybackEngine - Basic Orchestration", "[playback][engine]")
{
  Mock<IAudioBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  AudioDevice device{.id = "test-device",
                     .displayName = "Test",
                     .description = "Test",
                     .isDefault = false,
                     .backendKind = BackendKind::None};

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
  When(Method(mockBackend, kind)).AlwaysReturn(BackendKind::None);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());

  PlaybackEngine engine(std::move(backendPtr), device, dispatcher);

  SECTION("Stop correctly cleans up backend")
  {
    engine.stop();
    Verify(Method(mockBackend, reset)).AtLeastOnce();
    Verify(Method(mockBackend, stop)).AtLeastOnce();
    Verify(Method(mockBackend, close)).AtLeastOnce();

    auto snap = engine.snapshot();
    REQUIRE(snap.state == TransportState::Idle);
  }

  SECTION("Backend error transitions to Error state")
  {
    PlaybackEngine::onBackendError(&engine, "Hardware failure");

    auto snap = engine.snapshot();
    REQUIRE(snap.state == TransportState::Error);
    REQUIRE(snap.statusText == "Hardware failure");
  }

  SECTION("Route ready updates snapshot")
  {
    PlaybackEngine::onRouteReady(&engine, "anchor-123");

    auto route = engine.routeSnapshot();
    REQUIRE(route.anchor.has_value());
    REQUIRE(route.anchor->id == "anchor-123");
  }
}

TEST_CASE("PlaybackEngine - Backend Swapping", "[playback][engine][hot-swap]")
{
  Mock<IAudioBackend> mockBackend1;
  Mock<IAudioBackend> mockBackend2;
  auto dispatcher = std::make_shared<MockDispatcher>();

  Fake(
    Method(mockBackend1, open), Method(mockBackend1, reset), Method(mockBackend1, stop), Method(mockBackend1, close));
  Fake(
    Method(mockBackend2, open), Method(mockBackend2, reset), Method(mockBackend2, stop), Method(mockBackend2, close));
  When(Method(mockBackend1, kind)).AlwaysReturn(BackendKind::None);
  When(Method(mockBackend2, kind)).AlwaysReturn(BackendKind::AlsaExclusive);

  auto backend1 = std::make_unique<MockBackendProxy>(mockBackend1.get());
  PlaybackEngine engine(
    std::move(backend1),
    {.id = "dev1", .displayName = "D1", .description = "D1", .isDefault = false, .backendKind = BackendKind::None},
    dispatcher);

  SECTION("Switching backend while idle")
  {
    auto backend2 = std::make_unique<MockBackendProxy>(mockBackend2.get());
    engine.setBackend(std::move(backend2),
                      {.id = "dev2",
                       .displayName = "D2",
                       .description = "D2",
                       .isDefault = false,
                       .backendKind = BackendKind::AlsaExclusive});

    Verify(Method(mockBackend1, reset)).Once();
    Verify(Method(mockBackend1, stop)).Once();
    Verify(Method(mockBackend1, close)).Once();

    auto snap = engine.snapshot();
    REQUIRE(snap.backend == BackendKind::AlsaExclusive);
    REQUIRE(snap.currentDeviceId == "dev2");
  }
}

TEST_CASE("PlaybackEngine - Graph Initialization", "[playback][engine][graph]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping Graph Integrity test");
    return;
  }

  Mock<IAudioBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  AudioDevice device{.id = "test-device",
                     .displayName = "Test",
                     .description = "Test",
                     .isDefault = false,
                     .backendKind = BackendKind::None};

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
  When(Method(mockBackend, kind)).AlwaysReturn(BackendKind::None);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  PlaybackEngine engine(std::move(backendPtr), device, dispatcher);

  TrackPlaybackDescriptor descriptor;
  descriptor.filePath = testFile.string();
  descriptor.title = "Test Title";
  descriptor.artist = "Test Artist";

  engine.play(descriptor);

  auto snap = engine.snapshot();

  SECTION("rs-decoder is present in the graph")
  {
    auto it = std::ranges::find(snap.graph.nodes, "rs-decoder", &rs::audio::AudioNode::id);
    REQUIRE(it != snap.graph.nodes.end());
    CHECK(it->type == AudioNodeType::Decoder);
    CHECK(it->format.has_value());
    CHECK(it->format->sampleRate == 44100);
  }

  SECTION("rs-engine is present in the graph")
  {
    auto it = std::ranges::find(snap.graph.nodes, "rs-engine", &rs::audio::AudioNode::id);
    REQUIRE(it != snap.graph.nodes.end());
    CHECK(it->type == AudioNodeType::Engine);
  }

  SECTION("rs-decoder is linked to rs-engine")
  {
    auto it = std::ranges::find_if(
      snap.graph.links, [](auto const& l) { return l.sourceId == "rs-decoder" && l.destId == "rs-engine"; });
    CHECK(it != snap.graph.links.end());
    if (it != snap.graph.links.end())
    {
      CHECK(it->isActive);
    }
  }

  engine.stop();
}

TEST_CASE("PlaybackEngine - PipeWire shared mode keeps native sample rate", "[playback][engine][pipewire]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping PipeWire shared format test");
    return;
  }

  Mock<IAudioBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  AudioDevice device{.id = "pipewire-shared",
                     .displayName = "PipeWire",
                     .description = "PipeWire Shared",
                     .isDefault = false,
                     .backendKind = BackendKind::PipeWire,
                     .capabilities = {.sampleRates = {48000}, .bitDepths = {16}, .channelCounts = {2}}};

  auto openedFormats = std::vector<AudioFormat>{};
  When(Method(mockBackend, open))
    .AlwaysDo(
      [&](AudioFormat const& format, AudioRenderCallbacks)
      {
        openedFormats.push_back(format);
        return rs::Result<>();
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
  When(Method(mockBackend, kind)).AlwaysReturn(BackendKind::PipeWire);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  auto engine = PlaybackEngine(std::move(backendPtr), device, dispatcher);

  TrackPlaybackDescriptor descriptor;
  descriptor.filePath = testFile.string();
  descriptor.title = "PipeWire Shared";
  descriptor.artist = "Test Artist";

  engine.play(descriptor);

  Verify(Method(mockBackend, reset)).AtLeastOnce();
  REQUIRE(openedFormats.size() >= 1);
  CHECK(openedFormats.back().sampleRate == 44100);
  CHECK(openedFormats.back().channels == 2);
  CHECK(openedFormats.back().bitDepth == 16);
  CHECK(engine.snapshot().state == TransportState::Playing);

  engine.stop();
}

TEST_CASE("PlaybackEngine - Unsupported backend sample rate fails without resampler", "[playback][engine][format]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping backend sample-rate validation test");
    return;
  }

  Mock<IAudioBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  AudioDevice device{.id = "alsa-exclusive",
                     .displayName = "ALSA",
                     .description = "ALSA Exclusive",
                     .isDefault = false,
                     .backendKind = BackendKind::AlsaExclusive,
                     .capabilities = {.sampleRates = {48000}, .bitDepths = {16}, .channelCounts = {2}}};

  auto openedFormats = std::vector<AudioFormat>{};
  When(Method(mockBackend, open))
    .AlwaysDo(
      [&](AudioFormat const& format, AudioRenderCallbacks)
      {
        openedFormats.push_back(format);
        return rs::Result<>();
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
  When(Method(mockBackend, kind)).AlwaysReturn(BackendKind::AlsaExclusive);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  auto engine = PlaybackEngine(std::move(backendPtr), device, dispatcher);

  TrackPlaybackDescriptor descriptor;
  descriptor.filePath = testFile.string();
  descriptor.title = "Unsupported Sample Rate";
  descriptor.artist = "Test Artist";

  engine.play(descriptor);

  Verify(Method(mockBackend, reset)).AtLeastOnce();
  auto const snap = engine.snapshot();
  REQUIRE(snap.state == TransportState::Error);
  CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
  REQUIRE(openedFormats.size() == 0);
}
