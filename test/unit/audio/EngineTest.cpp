// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Engine.h>

#include "BackendTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  using namespace fakeit;

  TEST_CASE("Engine - stop while idle leaves an unopened backend untouched", "[audio][unit][engine]")
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
    Verify(Method(mockBackend, stop)).Never();
    Verify(Method(mockBackend, close)).Never();

    auto snap = engine.status();
    CHECK(snap.transport == Transport::Idle);
  }

  TEST_CASE("Engine - setBackend while idle replaces an unopened backend", "[audio][unit][engine][hot-swap]")
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

      Verify(Method(mockBackend1, stop)).Never();
      Verify(Method(mockBackend1, close)).Never();

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

    engine.play(makePlaybackItem(descriptor));

    auto const snap = engine.status();

    SECTION("AudioRouteFormatState reports decoder source format")
    {
      CHECK(snap.routeState.sourceFormat.sampleRate == 44100);
      CHECK(snap.routeState.sourceFormat.channels == 2);
      CHECK(snap.routeState.sourceFormat.precisionBits == 16);
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

  TEST_CASE("Engine - backend opens from the inspected native signal", "[audio][unit][engine][backend-open]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"pipewire-shared"},
                               .displayName = "PipeWire",
                               .description = "PipeWire Shared",
                               .isDefault = false,
                               .backendId = kBackendPipeWire};

    auto openedSignals = std::vector<SignalFormat>{};

    When(Method(mockBackend, open))
      .AlwaysDo(
        [&](SignalFormat const& sourceFormat, RenderTarget*&) -> Result<OpenedPcmMode>
        {
          openedSignals.push_back(sourceFormat);
          return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, SampleEncoding::Signed16Le)};
        });
    When(Method(mockBackend, backendId)).AlwaysReturn(kBackendPipeWire);

    auto engine = Engine{spy.makeProxy(), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(makePlaybackItem(descriptor));

    REQUIRE(openedSignals.size() == 1U);
    CHECK(openedSignals.back().sampleRate == 44100);
    CHECK(openedSignals.back().channels == 2);
    CHECK(openedSignals.back().precisionBits == 16);
    CHECK(engine.status().transport == Transport::Playing);

    engine.stop();
  }

  TEST_CASE("Engine - backend result selects the decoder PCM output", "[audio][regression][engine][backend-open]")
  {
    auto const nativeFormat = PcmFormat{.sampleRate = 96000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto const device = Device{.id = DeviceId{"wasapi-shared"},
                               .displayName = "WASAPI",
                               .description = "WASAPI Shared",
                               .isDefault = false,
                               .backendId = kBackendWasapi};
    auto openedSignals = std::vector<SignalFormat>{};
    auto decoderRequests = std::vector<std::optional<SampleEncoding>>{};
    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    When(Method(mockBackend, open))
      .AlwaysDo(
        [&](SignalFormat const& sourceFormat, RenderTarget*&) -> Result<OpenedPcmMode>
        {
          openedSignals.push_back(sourceFormat);
          return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, SampleEncoding::Signed24In32Le)};
        });
    When(Method(mockBackend, backendId)).AlwaysReturn(kBackendWasapi);
    When(Method(mockBackend, profileId)).AlwaysReturn(kProfileShared);

    auto decoderFactory = [nativeFormat, &decoderRequests](
                            std::filesystem::path const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.push_back(optOutputEncoding);
      auto const sourceFormat = signalFormat(nativeFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
        .duration = std::chrono::milliseconds{10},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(400, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto engine = Engine{spy.makeProxy(), device, std::move(decoderFactory)};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "native-96k.flac"}));

    REQUIRE(openedSignals.size() == 1);
    CHECK(openedSignals.front() == signalFormat(nativeFormat));
    REQUIRE(decoderRequests.size() == 2);
    CHECK_FALSE(decoderRequests.front());
    CHECK(decoderRequests.back() == SampleEncoding::Signed24In32Le);
    auto const status = engine.status();
    CHECK(status.transport == Transport::Playing);
    CHECK(status.routeState.sourceFormat == signalFormat(nativeFormat));
    auto const selectedFormat = pcmFormat(signalFormat(nativeFormat), SampleEncoding::Signed24In32Le);
    CHECK(status.routeState.decoderOutputFormat == selectedFormat);
    CHECK(status.routeState.engineOutputFormat == selectedFormat);
    engine.stop();
  }

  TEST_CASE("Engine - commit reuses a staged decoder when the backend prewarm hint matches",
            "[audio][regression][engine][staged]")
  {
    auto const nativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto decoderRequests = std::vector<std::optional<SampleEncoding>>{};
    auto decoderFactory = [nativeFormat, &decoderRequests](
                            std::filesystem::path const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.push_back(optOutputEncoding);
      auto const sourceFormat = signalFormat(nativeFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(3000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setPrewarmEncoding(SampleEncoding::Signed24In32Le);
    backendPtr->setSelectedEncoding(SampleEncoding::Signed24In32Le);
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), std::move(decoderFactory)};

    auto staged = engine.stagePlayback(makePlaybackItem("native-24.flac"));

    REQUIRE(staged);
    REQUIRE(decoderRequests.size() == 2);
    CHECK_FALSE(decoderRequests[0]);
    CHECK(decoderRequests[1] == SampleEncoding::Signed24In32Le);

    auto committed = engine.commitPlayback(std::move(*staged));

    REQUIRE(committed);
    CHECK(committed->playbackStarted);
    CHECK(decoderRequests.size() == 2);
    CHECK(engine.status().routeState.decoderOutputFormat ==
          pcmFormat(signalFormat(nativeFormat), SampleEncoding::Signed24In32Le));
    engine.stop();
  }

  TEST_CASE("Engine - a lossy prewarm hint is ignored", "[audio][regression][engine][staged]")
  {
    auto const nativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto decoderRequests = std::vector<std::optional<SampleEncoding>>{};
    auto decoderFactory = [nativeFormat, &decoderRequests](
                            std::filesystem::path const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.push_back(optOutputEncoding);
      auto const sourceFormat = signalFormat(nativeFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(3000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setPrewarmEncoding(SampleEncoding::Signed16Le);
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), std::move(decoderFactory)};

    auto staged = engine.stagePlayback(makePlaybackItem("native-24.flac"));

    REQUIRE(staged);
    REQUIRE(decoderRequests.size() == 1);
    CHECK_FALSE(decoderRequests.front());

    auto committed = engine.commitPlayback(std::move(*staged));

    REQUIRE(committed);
    CHECK(committed->playbackStarted);
    REQUIRE(decoderRequests.size() == 2);
    CHECK(decoderRequests.back() == SampleEncoding::Signed24PackedLe);
    CHECK(engine.status().routeState.decoderOutputFormat.encoding == SampleEncoding::Signed24PackedLe);
    engine.stop();
  }

  TEST_CASE("Engine - explicit staging follows the backend hint after a wider current PCM mode",
            "[audio][regression][engine][staged]")
  {
    auto const currentNativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto const successorNativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto decoderRequests = std::vector<std::pair<std::filesystem::path, std::optional<SampleEncoding>>>{};
    auto decoderFactory = [currentNativeFormat, successorNativeFormat, &decoderRequests](
                            std::filesystem::path const& path, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.emplace_back(path, optOutputEncoding);
      auto const nativeFormat = path == "current-24.flac" ? currentNativeFormat : successorNativeFormat;
      auto const sourceFormat = signalFormat(nativeFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(4000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto engine = Engine{std::make_unique<FakeCapturingBackend>(), makeEngineTestDevice(), std::move(decoderFactory)};
    engine.play(makePlaybackItem("current-24.flac"));
    REQUIRE(engine.status().transport == Transport::Playing);
    decoderRequests.clear();

    auto staged = engine.stagePlayback(makePlaybackItem("successor-16.flac"));

    REQUIRE(staged);
    REQUIRE(decoderRequests.size() == 2);
    CHECK_FALSE(decoderRequests[0].second);
    CHECK(decoderRequests[1].second == SampleEncoding::Signed16Le);

    auto committed = engine.commitPlayback(std::move(*staged));

    REQUIRE(committed);
    CHECK(committed->playbackStarted);
    CHECK(decoderRequests.size() == 2);
    CHECK(engine.status().routeState.decoderOutputFormat.encoding == SampleEncoding::Signed16Le);
    engine.stop();
  }

  TEST_CASE("Engine - commit replaces a staged decoder when the backend selects another lossless mode",
            "[audio][regression][engine][staged]")
  {
    auto const nativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto decoderRequests = std::vector<std::optional<SampleEncoding>>{};
    auto decoderFactory = [nativeFormat, &decoderRequests](
                            std::filesystem::path const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      decoderRequests.push_back(optOutputEncoding);
      auto const sourceFormat = signalFormat(nativeFormat);
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(4000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setSelectedEncoding(SampleEncoding::Signed24In32Le);
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), std::move(decoderFactory)};
    auto staged = engine.stagePlayback(makePlaybackItem("fallback-24.flac"));
    REQUIRE(staged);
    REQUIRE(decoderRequests.size() == 2);
    CHECK_FALSE(decoderRequests[0]);
    CHECK(decoderRequests[1] == SampleEncoding::Signed24PackedLe);

    auto committed = engine.commitPlayback(std::move(*staged));

    REQUIRE(committed);
    CHECK(committed->playbackStarted);
    REQUIRE(decoderRequests.size() == 3);
    CHECK(decoderRequests[2] == SampleEncoding::Signed24In32Le);
    CHECK(engine.status().routeState.decoderOutputFormat.encoding == SampleEncoding::Signed24In32Le);
    engine.stop();
  }

  TEST_CASE("Engine - AAC playback supports 32-bit padded backend output", "[audio][unit][engine][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto const device = Device{.id = DeviceId{"alsa-exclusive"},
                               .displayName = "ALSA",
                               .description = "ALSA Exclusive",
                               .isDefault = false,
                               .backendId = kBackendAlsa};
    backendRaw->setSelectedEncoding(SampleEncoding::Signed32Le);

    auto engine = Engine{std::move(backendPtr), device};
    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(makePlaybackItem(descriptor));

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Playing);

    auto const events = backendRaw->events();
    REQUIRE(!events.empty());
    auto optOpenFormat = std::optional<PcmFormat>{};

    for (auto const& event : events)
    {
      if (event.name == "open")
      {
        optOpenFormat = event.format;
        break;
      }
    }

    REQUIRE(optOpenFormat);
    CHECK(optOpenFormat->encoding == SampleEncoding::Signed32Le);
    CHECK(optOpenFormat->sampleRate == 44100);

    engine.stop();
  }

  TEST_CASE("Engine - backend format rejection is reported without fallback", "[audio][unit][engine][format]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"alsa-exclusive"},
                               .displayName = "ALSA",
                               .description = "ALSA Exclusive",
                               .isDefault = false,
                               .backendId = kBackendAlsa};

    auto openedSignals = std::vector<SignalFormat>{};

    When(Method(mockBackend, open))
      .AlwaysDo(
        [&](SignalFormat const& sourceFormat, RenderTarget*&) -> Result<OpenedPcmMode>
        {
          openedSignals.push_back(sourceFormat);
          return makeError(Error::Code::FormatRejected, "test backend rejected native sample rate");
        });
    When(Method(mockBackend, backendId)).AlwaysReturn(kBackendAlsa);
    When(Method(mockBackend, profileId)).AlwaysReturn(kProfileExclusive);

    auto engine = Engine{spy.makeProxy(), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};

    engine.play(makePlaybackItem(descriptor));

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText.contains("rejected native sample rate"));
    REQUIRE(openedSignals.size() == 1U);
    CHECK(openedSignals.front().sampleRate == 44100U);
  }

  TEST_CASE("Engine - pause and resume update backend transport", "[audio][unit][engine][transport]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    auto const fmt = makeEngineTestFormat();
    auto const factory = [fmt](auto const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = signalFormat(fmt);
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = sourceFormat,
                          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(fmt.encoding)),
                          .duration = std::chrono::milliseconds{0},
                          .isLossy = false});

      // provide some data for preroll
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{.data = data, .endOfStream = false}, {.endOfStream = true}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    engine.play(makePlaybackItem(desc));
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
    auto backendPtr = std::make_unique<FakeCapturingBackend>();

    auto const fmt =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le}; // 2 bytes = 1ms
    auto const factory = [fmt](auto const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = signalFormat(fmt);
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = sourceFormat,
                          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(fmt.encoding)),
                          .duration = std::chrono::milliseconds{0},
                          .isLossy = false});
      auto data = std::vector(200, std::byte{0}); // 100ms

      decPtr->setReadScript(
        {{.data = data, .endOfStream = false}, {.data = data, .endOfStream = false}, {.endOfStream = true}});
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
      engine.play(makePlaybackItem(desc));
      engine.seek(std::chrono::milliseconds{50});
      auto const snapshot = engine.status();
      CHECK(snapshot.elapsed == std::chrono::milliseconds{50});
      CHECK(snapshot.transport == Transport::Playing);
      CHECK(snapshot.bufferedDuration == std::chrono::milliseconds{200});
    }
  }

  TEST_CASE("Engine - play with initial offset seeks before publishing elapsed", "[audio][unit][engine][seek]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto orderedEvents = std::vector<std::string>{};
    backendRaw->setEventObserver([&orderedEvents](std::string_view name) { orderedEvents.emplace_back(name); });
    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();
    auto const fmt = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const data = std::vector(200, std::byte{0});
    auto const path = std::filesystem::path{"offset.flac"};
    auto info = makeScriptedStreamInfo(fmt);
    info.duration = std::chrono::milliseconds{100};
    auto factory = [info, data, path, registryPtr, &orderedEvents](
                     std::filesystem::path const& requestedPath, std::optional<SampleEncoding> optOutputEncoding)
    {
      if (requestedPath != path)
      {
        return std::unique_ptr<ScriptedDecoderSession>{};
      }

      auto requestedInfo = info;
      requestedInfo.outputFormat = pcmFormat(info.sourceFormat, optOutputEncoding.value_or(info.outputFormat.encoding));
      auto decPtr = std::make_unique<ScriptedDecoderSession>(requestedInfo);
      decPtr->setReadScript({{.data = data, .endOfStream = false}, {.endOfStream = true}});
      decPtr->setSeekObserver([&orderedEvents](std::chrono::milliseconds) { orderedEvents.emplace_back("seek"); });
      (*registryPtr)[path] = decPtr.get();
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, std::move(factory)};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = path}), std::chrono::milliseconds{50});

    REQUIRE(registryPtr->contains(path));
    auto* const decoder = registryPtr->at(path);
    REQUIRE(decoder != nullptr);
    CHECK(decoder->seekCount() == 1);
    CHECK(decoder->lastSeekOffset() == std::chrono::milliseconds{50});
    CHECK(engine.status().elapsed == std::chrono::milliseconds{50});
    CHECK(engine.status().transport == Transport::Playing);

    auto const events = backendRaw->events();
    REQUIRE(!events.empty());
    CHECK(events.back().name == "start");

    auto const seekIt = std::ranges::find(orderedEvents, std::string_view{"seek"});
    auto const openIt = std::ranges::find(orderedEvents, std::string_view{"open"});
    auto const startIt = std::ranges::find(orderedEvents, std::string_view{"start"});
    REQUIRE(seekIt != orderedEvents.end());
    REQUIRE(openIt != orderedEvents.end());
    REQUIRE(startIt != orderedEvents.end());
    CHECK(openIt < seekIt);
    CHECK(seekIt < startIt);
  }

  namespace
  {
    // Two tracks whose native encodings can be chosen per path, so a lookahead
    // can be pointed at a successor that either matches the current signal or
    // deliberately does not.
    auto makeTwoTrackFactory(
      std::map<std::filesystem::path, PcmFormat> nativeFormats,
      std::vector<std::pair<std::filesystem::path, std::optional<SampleEncoding>>>& decoderRequests)
    {
      return [nativeFormats = std::move(nativeFormats), &decoderRequests](
               std::filesystem::path const& path, std::optional<SampleEncoding> optOutputEncoding)
      {
        decoderRequests.emplace_back(path, optOutputEncoding);
        auto const nativeFormat = nativeFormats.at(path);
        auto const sourceFormat = signalFormat(nativeFormat);
        auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = sourceFormat,
          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(nativeFormat.encoding)),
          .duration = std::chrono::seconds{2},
          .isLossy = false,
          .codec = AudioCodec::Flac,
        });
        decoderPtr->setReadScript(
          {{.data = std::vector<std::byte>(4000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
        return decoderPtr;
      };
    }
  } // namespace

  TEST_CASE("Engine - a confirmed endpoint does not admit a reduced client format", "[audio][regression][engine]")
  {
    auto const nativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto decoderRequests = std::vector<std::pair<std::filesystem::path, std::optional<SampleEncoding>>>{};
    auto decoderFactory = makeTwoTrackFactory({{"only-16.flac", nativeFormat}}, decoderRequests);

    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setSelectedEncoding(SampleEncoding::Signed16Le);
    backendPtr->setConfirmedEndpointPrecision(std::uint8_t{16});
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), std::move(decoderFactory)};

    engine.play(makePlaybackItem("only-16.flac"));

    CHECK(engine.status().transport == Transport::Error);
    engine.stop();
  }

  TEST_CASE("Engine - a lossy format without endpoint evidence is refused", "[audio][regression][engine]")
  {
    auto const nativeFormat =
      PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};
    auto decoderRequests = std::vector<std::pair<std::filesystem::path, std::optional<SampleEncoding>>>{};
    auto decoderFactory = makeTwoTrackFactory({{"only-16.flac", nativeFormat}}, decoderRequests);

    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setSelectedEncoding(SampleEncoding::Signed16Le);
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), std::move(decoderFactory)};

    engine.play(makePlaybackItem("only-16.flac"));

    CHECK(engine.status().transport == Transport::Error);
    engine.stop();
  }
} // namespace ao::audio::test
