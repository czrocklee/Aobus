// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include "TestUtility.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

    engine.play(makePlaybackItem(descriptor));

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

    engine.play(makePlaybackItem(descriptor));

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

    engine.play(makePlaybackItem(descriptor));

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

  TEST_CASE("Engine - unsupported backend sample rate fails without resampler", "[audio][unit][engine][format]")
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

    engine.play(makePlaybackItem(descriptor));

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText.find("no resampler yet") != std::string::npos);
    CHECK(openedFormats.empty());
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
      engine.play(makePlaybackItem(desc));
      engine.seek(std::chrono::milliseconds{50});
      CHECK(engine.status().elapsed == std::chrono::milliseconds{50});
      CHECK(engine.status().transport == Transport::Playing);
    }
  }

  TEST_CASE("Engine - play with initial offset seeks before publishing elapsed", "[audio][unit][engine][seek]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto orderedEvents = std::vector<std::string>{};
    backendRaw->setEventObserver([&orderedEvents](std::string_view name) { orderedEvents.emplace_back(name); });
    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();
    auto const fmt = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const data = std::vector(200, std::byte{0});
    auto const path = std::filesystem::path{"offset.flac"};
    auto info = makeScriptedStreamInfo(fmt);
    info.duration = std::chrono::milliseconds{100};
    auto factory = [info, data, path, registryPtr, &orderedEvents](
                     std::filesystem::path const& requestedPath, Format const&)
    {
      if (requestedPath != path)
      {
        return std::unique_ptr<ScriptedDecoderSession>{};
      }

      auto decPtr = std::make_unique<ScriptedDecoderSession>(info);
      decPtr->setReadScript({{data, false}, {{}, true}});
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
    CHECK(seekIt < openIt);
    CHECK(seekIt < startIt);
  }
} // namespace ao::audio::test
