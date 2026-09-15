// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/backend/AlsaMixerTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  TEST_CASE("Engine - ALSA readback keeps application mute across open swap and stop graph observations",
            "[audio][regression][alsa][concurrency]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->optHardwareMuted = true;
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto registry = detail::AlsaGraphRegistry{};
    auto const firstDevice = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backendPtr =
      std::make_unique<detail::test::AlsaControlBackend>(firstDevice, registry.publisher(), mixerStatePtr);
    REQUIRE(backendPtr->isMixerInitialized());
    enum class ObservationStage : std::uint8_t
    {
      None,
      Open,
      Stop,
      Swap,
    };

    auto* engine = static_cast<Engine*>(nullptr);
    struct ObservedControls final
    {
      float volume = 0.0F;
      bool muted = false;
      bool available = false;
    };

    auto observedControls = std::vector<ObservedControls>{};
    auto stage = ObservationStage::None;
    std::size_t firstGraphCount = 0U;
    std::size_t openStatusReadCount = 0U;
    std::size_t stopStatusReadCount = 0U;
    std::size_t swapStatusReadCount = 0U;
    auto observeGraph = [&](flow::Graph const&)
    {
      ++firstGraphCount;

      if (engine == nullptr)
      {
        return;
      }

      observedControls.push_back(
        {.volume = engine->volume(), .muted = engine->isMuted(), .available = engine->isVolumeAvailable()});

      switch (stage)
      {
        case ObservationStage::None: break;
        case ObservationStage::Open: ++openStatusReadCount; break;
        case ObservationStage::Stop: ++stopStatusReadCount; break;
        case ObservationStage::Swap: ++swapStatusReadCount; break;
      }
    };
    auto graphSub = registry.subscribe(firstDevice.id.raw(), observeGraph);
    auto const refreshCountBeforeEngine = mixerStatePtr->refreshCount;
    auto audioEngine =
      Engine{std::move(backendPtr), firstDevice, ::ao::audio::test::makeScriptedEngineDecoderFactory()};
    engine = &audioEngine;

    CHECK(mixerStatePtr->refreshCount == refreshCountBeforeEngine + 1U);
    CHECK_FALSE(audioEngine.status().muted);
    // Opening must observe a real graph change, not depend on an identical snapshot being redelivered.
    mixerStatePtr->hardwareElements.front().rawLevels = {100L};
    stage = ObservationStage::Open;
    audioEngine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "test.flac"}));
    stage = ObservationStage::None;
    CHECK_FALSE(audioEngine.status().muted);
    CHECK(audioEngine.volume() == 1.0F);
    CHECK(openStatusReadCount > 0U);

    REQUIRE(audioEngine.setMuted(true));
    CHECK(audioEngine.status().muted);
    auto const graphCountBeforeMutedStop = firstGraphCount;
    stage = ObservationStage::Stop;
    audioEngine.stop();
    stage = ObservationStage::None;
    CHECK(firstGraphCount == graphCountBeforeMutedStop + 1U);
    CHECK(stopStatusReadCount > 0U);
    CHECK(audioEngine.status().muted);
    stage = ObservationStage::Open;
    audioEngine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "muted-reopen.flac"}));
    stage = ObservationStage::None;
    CHECK(audioEngine.status().muted);

    REQUIRE(audioEngine.setMuted(false));
    CHECK_FALSE(audioEngine.status().muted);
    CHECK(mixerStatePtr->optHardwareMuted == true);
    CHECK(mixerStatePtr->writeCount == 0U);

    auto const graphCountBeforeUnmutedStop = firstGraphCount;
    stage = ObservationStage::Stop;
    audioEngine.stop();
    stage = ObservationStage::None;
    CHECK(firstGraphCount == graphCountBeforeUnmutedStop + 1U);
    CHECK_FALSE(audioEngine.status().muted);
    stage = ObservationStage::Open;
    audioEngine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "unmuted-reopen.flac"}));
    stage = ObservationStage::None;
    CHECK_FALSE(audioEngine.status().muted);

    auto nextMixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    nextMixerStatePtr->optHardwareMuted = true;
    nextMixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {60L}});
    auto nextRegistry = detail::AlsaGraphRegistry{};
    auto const nextDevice = Device{
      .id = DeviceId{"hw:test,1"}, .displayName = "Next card", .description = "hw:test,1", .backendId = kBackendAlsa};
    auto nextBackendPtr =
      std::make_unique<detail::test::AlsaControlBackend>(nextDevice, nextRegistry.publisher(), nextMixerStatePtr);
    REQUIRE(nextBackendPtr->isMixerInitialized());
    auto const observationsBeforeSubscribe = observedControls.size();
    auto nextGraphSub = nextRegistry.subscribe(nextDevice.id.raw(), observeGraph);
    REQUIRE(observedControls.size() == observationsBeforeSubscribe + 1U);
    CHECK_FALSE(observedControls.back().muted);
    CHECK(observedControls.back().available);
    CHECK(observedControls.back().volume == audioEngine.volume());

    stage = ObservationStage::Swap;
    audioEngine.setBackend(std::move(nextBackendPtr), nextDevice);
    stage = ObservationStage::None;

    auto const statusAfterSwap = audioEngine.status();
    CHECK(statusAfterSwap.currentDeviceId == nextDevice.id);
    CHECK_FALSE(statusAfterSwap.muted);
    CHECK(openStatusReadCount > 0U);
    CHECK(stopStatusReadCount > 0U);
    CHECK(swapStatusReadCount > 0U);
    CHECK(observedControls.size() >= openStatusReadCount + stopStatusReadCount + swapStatusReadCount);
    CHECK(nextMixerStatePtr->writeCount == 0U);
  }

  TEST_CASE("Engine - ALSA stop preserves successful volume and application mute without another mixer write",
            "[audio][regression][alsa]")
  {
    bool const hardware = GENERATE(false, true);
    bool const muted = GENERATE(false, true);
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->openSucceeds = hardware;
    mixerStatePtr->hardwareElements.push_back(
      {.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 0L, .max = 100L}, .rawLevels = {100L}});
    auto registry = detail::AlsaGraphRegistry{};
    auto const device = ::ao::audio::test::makeEngineTestDevice("hw:test,0");
    auto engine =
      Engine{std::make_unique<detail::test::AlsaControlBackend>(device, registry.publisher(), mixerStatePtr),
             device,
             ::ao::audio::test::makeScriptedEngineDecoderFactory()};
    engine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "volume-stop.flac"}));
    REQUIRE(engine.status().volumeAvailable);
    REQUIRE(engine.status().volumeIsHardwareAssisted == hardware);
    REQUIRE(engine.setVolume(0.25F));
    REQUIRE(engine.setMuted(muted));
    REQUIRE(engine.volume() == 0.25F);
    REQUIRE(engine.isMuted() == muted);
    auto const writesBeforeStop = mixerStatePtr->writeCount;
    REQUIRE(writesBeforeStop == (hardware ? 1U : 0U));

    engine.stop();

    auto const stopped = engine.status();
    CHECK(stopped.volume == 0.25F);
    CHECK(engine.volume() == 0.25F);
    CHECK(stopped.muted == muted);
    CHECK(engine.isMuted() == muted);
    CHECK_FALSE(stopped.volumeAvailable);
    CHECK_FALSE(stopped.volumeIsHardwareAssisted);
    CHECK(mixerStatePtr->writeCount == writesBeforeStop);
    CHECK(mixerStatePtr->hardwareElements.front().rawLevels == std::vector<long>{hardware ? 25L : 100L});

    engine.stop();
    CHECK(engine.volume() == 0.25F);
    CHECK(engine.isMuted() == muted);
    CHECK(mixerStatePtr->writeCount == writesBeforeStop);
  }

  TEST_CASE("Engine - ALSA stop preserves rejected volume intent without another hardware write",
            "[audio][regression][alsa]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back(
      {.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 0L, .max = 100L}, .rawLevels = {100L, 80L}});
    auto registry = detail::AlsaGraphRegistry{};
    auto const device = ::ao::audio::test::makeEngineTestDevice("hw:test,0");
    auto engine =
      Engine{std::make_unique<detail::test::AlsaControlBackend>(device, registry.publisher(), mixerStatePtr),
             device,
             ::ao::audio::test::makeScriptedEngineDecoderFactory()};
    engine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "volume-failure.flac"}));
    REQUIRE(engine.status().volumeIsHardwareAssisted);
    REQUIRE(engine.setMuted(true));
    mixerStatePtr->writeSucceeds = false;
    mixerStatePtr->applyFirstChannelBeforeWriteFailure = true;

    auto const volumeRes = engine.setVolume(0.25F);

    REQUIRE_FALSE(volumeRes);
    CHECK(engine.volume() == 0.25F);
    CHECK(engine.isMuted());
    CHECK(engine.status().volumeAvailable);
    CHECK_FALSE(engine.status().volumeIsHardwareAssisted);
    engine.stop();
    CHECK(engine.volume() == 0.25F);
    CHECK(engine.isMuted());
    CHECK_FALSE(engine.status().volumeAvailable);
    CHECK(mixerStatePtr->writeCount == 1U);
    CHECK((mixerStatePtr->hardwareElements.front().rawLevels == std::vector<long>{25L, 80L}));
  }

  TEST_CASE("Engine - ALSA property refresh fallback publishes a graph and current capability",
            "[audio][regression][alsa][concurrency]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto registry = detail::AlsaGraphRegistry{};
    auto const device = ::ao::audio::test::makeEngineTestDevice("hw:test,0");
    auto engine =
      Engine{std::make_unique<detail::test::AlsaControlBackend>(device, registry.publisher(), mixerStatePtr),
             device,
             ::ao::audio::test::makeScriptedEngineDecoderFactory()};
    auto graph = flow::Graph{};
    std::size_t stateReadCount = 0U;
    bool observedMuted = true;
    auto graphSub = registry.subscribe(device.id.raw(),
                                       [&](flow::Graph const& nextGraph)
                                       {
                                         graph = nextGraph;
                                         observedMuted = engine.isMuted();
                                         ++stateReadCount;
                                       });
    REQUIRE(engine.status().volumeIsHardwareAssisted);
    REQUIRE(graph.nodes.size() == 2U);
    REQUIRE(graph.nodes.back().hardwareVolumeNotUnity);
    REQUIRE(stateReadCount == 1U);
    std::size_t refreshesAfterClose = 0U;
    mixerStatePtr->beforeRefresh = [&]
    {
      // Reopen selects hardware successfully; only the subsequent property observation fails.
      if (++refreshesAfterClose == 2U)
      {
        mixerStatePtr->refreshSucceeds = false;
      }
    };

    engine.play(::ao::audio::test::makePlaybackItem(PlaybackInput{.filePath = "read-fallback.flac"}));
    mixerStatePtr->beforeRefresh = {};

    auto const status = engine.status();
    CHECK(refreshesAfterClose == 2U);
    CHECK(status.volumeAvailable);
    CHECK_FALSE(status.volumeIsHardwareAssisted);
    CHECK(status.volume == 1.0F);
    CHECK_FALSE(status.muted);
    CHECK_FALSE(observedMuted);
    CHECK(stateReadCount > 1U);
    REQUIRE(graph.nodes.size() == 2U);
    CHECK_FALSE(graph.nodes.back().hardwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes.back().softwareVolumeNotUnity);
    CHECK(mixerStatePtr->writeCount == 0U);
  }
} // namespace ao::audio::backend::test
