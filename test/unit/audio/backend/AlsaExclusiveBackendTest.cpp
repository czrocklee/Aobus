// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "lib/audio/backend/detail/AlsaMixerSession.h"
#include "test/unit/audio/BackendTestSupport.h"
#include "test/unit/audio/backend/AlsaMixerTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - non-hardware PCM is rejected", "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code == Error::Code::FormatRejected);
    CHECK(openedRes.error().message.contains("direct hardware PCM"));
  }

  TEST_CASE("AlsaExclusiveBackend - unsupported signal is rejected before native device inspection",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code == Error::Code::NotSupported);
    CHECK(openedRes.error().message == "No lossless PCM encoding is available for ALSA");
  }

  TEST_CASE("AlsaExclusiveBackend - a missing device is reported as such, never as a format problem",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // A failed native open must surface its own cause. Translating it into a
    // format decision is exactly how an unavailable device once turned into a
    // silent 16-bit fallback.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code != Error::Code::FormatRejected);
    CHECK(openedRes.error().message.contains("Failed to open ALSA device"));
  }

  TEST_CASE("AlsaExclusiveBackend - failed open leaves prewarm hint on immutable policy", "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Nothing is cached before a successful open, so a device that was never
    // opened predicts only from immutable policy.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};

    auto const openedRes = backend.open(sourceFormat, target);
    REQUIRE_FALSE(openedRes);

    auto const optHint = backend.prewarmFormatHint(sourceFormat);

    REQUIRE(optHint);
    CHECK(optHint->sampleRate == 48000);
    CHECK(optHint->channels == 2);
    CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
  }

  TEST_CASE("AlsaExclusiveBackend - NaN volume does not observe or publish mixer state", "[audio][unit][alsa]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {50L}});
    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr));
    auto registry = detail::AlsaGraphRegistry{};
    std::size_t graphUpdateCount = 0U;
    auto graphSub = registry.subscribe("hw:127,0", [&](flow::Graph const&) { ++graphUpdateCount; });
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};
    auto const refreshCountBeforeNaN = mixerStatePtr->refreshCount;
    auto const updateCountBeforeNaN = graphUpdateCount;

    auto const nanRes = backend.set(props::kVolume, std::numeric_limits<float>::quiet_NaN());

    REQUIRE_FALSE(nanRes);
    CHECK(nanRes.error().code == Error::Code::InvalidInput);
    CHECK(mixerStatePtr->refreshCount == refreshCountBeforeNaN);
    CHECK(mixerStatePtr->writeCount == 0U);
    CHECK(graphUpdateCount == updateCountBeforeNaN);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);
  }

  TEST_CASE("AlsaExclusiveBackend - property observation publishes effective mixer state and fallback",
            "[audio][unit][alsa]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->optHardwareMuted = true;
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr));
    auto registry = detail::AlsaGraphRegistry{};
    auto graph = flow::Graph{};
    std::size_t graphUpdateCount = 0U;
    auto graphSub = registry.subscribe("hw:test,0",
                                       [&](flow::Graph const& nextGraph)
                                       {
                                         graph = nextGraph;
                                         ++graphUpdateCount;
                                       });
    auto const device = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};
    auto const refreshCountBeforeMuteRead = mixerStatePtr->refreshCount;

    auto const mutedRes = backend.property(PropertyId::Muted);

    REQUIRE(mutedRes);
    CHECK_FALSE(std::get<bool>(*mutedRes));
    CHECK(mixerStatePtr->refreshCount == refreshCountBeforeMuteRead);
    CHECK(graphUpdateCount == 1U);

    auto constexpr kUnknownProperty = static_cast<PropertyId>(999);
    auto const unknownRes = backend.property(kUnknownProperty);
    REQUIRE_FALSE(unknownRes);
    CHECK(unknownRes.error().code == Error::Code::NotSupported);
    CHECK(mixerStatePtr->refreshCount == refreshCountBeforeMuteRead);
    CHECK(graphUpdateCount == 1U);

    auto const volumeRes = backend.property(PropertyId::Volume);

    REQUIRE(volumeRes);
    CHECK(std::get<float>(*volumeRes) == 0.8F);
    REQUIRE(graphUpdateCount == 2U);
    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes.back().isMuted);
    CHECK(graph.nodes.back().hardwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes.back().softwareVolumeNotUnity);
    CHECK(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);

    REQUIRE(backend.set(props::kMuted, true));
    CHECK(graph.nodes.back().isMuted);
    REQUIRE(backend.set(props::kMuted, false));
    CHECK(graph.nodes.back().isMuted);
    CHECK(mixerStatePtr->writeCount == 0U);
    CHECK(mixerStatePtr->optHardwareMuted == true);

    mixerStatePtr->hardwareElements.clear();
    auto const updateCountBeforeFallback = graphUpdateCount;
    auto const fallbackVolumeRes = backend.property(PropertyId::Volume);

    REQUIRE(fallbackVolumeRes);
    CHECK(graphUpdateCount == updateCountBeforeFallback + 1U);
    CHECK(std::get<float>(*fallbackVolumeRes) == 1.0F);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);
    REQUIRE(graph.nodes.size() == 2U);
    CHECK_FALSE(graph.nodes.back().isMuted);
    CHECK_FALSE(graph.nodes.back().hardwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes.back().softwareVolumeNotUnity);

    backend.close();
    auto const updateCountAfterClose = graphUpdateCount;
    REQUIRE(graph.nodes.empty());

    auto const closedVolumeRes = backend.property(PropertyId::Volume);
    auto const closedMutedRes = backend.property(PropertyId::Muted);

    REQUIRE(closedVolumeRes);
    REQUIRE(closedMutedRes);
    CHECK_FALSE(std::get<bool>(*closedMutedRes));
    CHECK(graphUpdateCount == updateCountAfterClose);
    CHECK(graph.nodes.empty());
  }

  TEST_CASE("AlsaExclusiveBackend - software fallback survives close and reopen after a mixer write failure",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {90L}});
    auto registry = detail::AlsaGraphRegistry{};
    auto graph = flow::Graph{};
    auto graphSub = registry.subscribe("hw:test,0", [&graph](flow::Graph const& nextGraph) { graph = nextGraph; });
    auto const device = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backend = detail::test::AlsaControlBackend{device, registry.publisher(), mixerStatePtr};
    mixerStatePtr->writeSucceeds = false;

    auto const failedWriteRes = backend.set(props::kVolume, 0.25F);
    REQUIRE_FALSE(failedWriteRes);
    CHECK(failedWriteRes.error().code == Error::Code::IoError);
    CHECK(failedWriteRes.error().message.contains("hardware state may have changed partially"));
    CHECK_FALSE(backend.isMixerInitialized());
    REQUIRE(graph.nodes.size() == 2U);
    CHECK_FALSE(graph.nodes.back().softwareVolumeNotUnity);
    CHECK(graph.nodes.back().minSoftwareGain == 1.0F);
    CHECK(graph.nodes.back().maxSoftwareGain == 1.0F);

    REQUIRE(backend.set(props::kVolume, 0.3F));
    auto const fallbackRes = backend.property(PropertyId::Volume);
    REQUIRE(fallbackRes);
    CHECK(std::get<float>(*fallbackRes) == 0.3F);
    backend.close();
    REQUIRE(graph.nodes.empty());
    mixerStatePtr->writeSucceeds = true;
    REQUIRE(backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16}, target));

    CHECK_FALSE(backend.isMixerInitialized());
    auto const volumeRes = backend.property(PropertyId::Volume);
    REQUIRE(volumeRes);
    CHECK(std::get<float>(*volumeRes) == 0.3F);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);
    REQUIRE(graph.nodes.size() == 2U);
    auto const& sink = graph.nodes.back();
    CHECK(sink.softwareVolumeNotUnity);
    CHECK_FALSE(sink.hardwareVolumeNotUnity);
    CHECK_FALSE(sink.unclassifiedVolumeNotUnity);
    CHECK(sink.minSoftwareGain == 0.3F);
    CHECK(sink.maxSoftwareGain == 0.3F);
    CHECK(mixerStatePtr->openCount == 2U);
    CHECK(mixerStatePtr->closeCount == 2U);
    CHECK(mixerStatePtr->writeCount == 1U);
  }

  TEST_CASE("AlsaExclusiveBackend - every graph callback reads volume without nested delivery", "[audio][unit][alsa]")
  {
    bool const hardware = GENERATE(false, true);
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();

    if (hardware)
    {
      mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {25L}});
    }

    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr) == hardware);

    if (!hardware)
    {
      REQUIRE(mixerPtr->setVolume(0.25F));
    }

    auto registry = detail::AlsaGraphRegistry{};
    auto const device = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};
    bool initialGraphIsCurrent = false;

    SECTION("initialized mixer without a stored graph")
    {
    }

    SECTION("current stored graph")
    {
      REQUIRE(backend.property(PropertyId::Volume));
      initialGraphIsCurrent = true;
    }

    SECTION("stale stored graph")
    {
      registry.publish({.routeAnchor = "hw:test,0",
                        .volume = 1.0F,
                        .volumeMode = hardware ? detail::AlsaVolumeControlMode::HardwareMixer
                                               : detail::AlsaVolumeControlMode::SoftwareGain});
    }

    auto graphs = std::vector<flow::Graph>{};
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    bool allReadsSucceeded = true;
    float observedVolume = -1.0F;
    auto graphSub = registry.subscribe("hw:test,0",
                                       [&](flow::Graph const& graph)
                                       {
                                         ++depth;
                                         maxDepth = std::max(maxDepth, depth);
                                         graphs.push_back(graph);

                                         // Bound the broken implementation's recursion, not normal observation.
                                         if (depth < 8U)
                                         {
                                           auto const volumeRes = backend.property(PropertyId::Volume);
                                           allReadsSucceeded = allReadsSucceeded && volumeRes.has_value();

                                           if (volumeRes)
                                           {
                                             observedVolume = std::get<float>(*volumeRes);
                                           }
                                         }

                                         --depth;
                                       });

    CHECK(maxDepth == 1U);
    CHECK(graphs.size() == (initialGraphIsCurrent ? 1U : 2U));
    CHECK(allReadsSucceeded);
    CHECK(observedVolume == 0.25F);
    REQUIRE_FALSE(graphs.empty());
    REQUIRE(graphs.back().nodes.size() == 2U);
    CHECK(graphs.back().nodes.back().hardwareVolumeNotUnity == hardware);
    CHECK(graphs.back().nodes.back().softwareVolumeNotUnity == !hardware);
    CHECK(backend.queryProperty(PropertyId::Volume).isHardwareAssisted == hardware);

    auto const stableCount = graphs.size();
    REQUIRE(backend.property(PropertyId::Volume));
    REQUIRE(backend.property(PropertyId::Volume));
    CHECK(graphs.size() == stableCount);

    // A genuine ordinary publication must still reach a subscriber that always reads volume.
    REQUIRE(backend.set(props::kMuted, true));
    CHECK(graphs.size() == stableCount + 1U);
    CHECK(graphs.back().nodes.back().isMuted);
    CHECK(maxDepth == 1U);

    auto const countBeforeClose = graphs.size();
    backend.close();
    CHECK(graphs.size() == countBeforeClose + 1U);
    CHECK(graphs.back().nodes.empty());
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK(maxDepth == 1U);
    CHECK(allReadsSucceeded);

    auto const countAfterClose = graphs.size();
    REQUIRE(backend.property(PropertyId::Volume));
    CHECK(graphs.size() == countAfterClose);
    CHECK(graphs.back().nodes.empty());
    CHECK(mixerStatePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaExclusiveBackend - initial callback volume read delivers discovered fallback after unwinding",
            "[audio][unit][alsa]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr));
    auto registry = detail::AlsaGraphRegistry{};
    auto const device = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};
    REQUIRE(backend.property(PropertyId::Volume));
    mixerStatePtr->hardwareElements.clear();
    auto graphs = std::vector<flow::Graph>{};
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    bool allReadsReturnedUnity = true;
    auto graphSub = registry.subscribe("hw:test,0",
                                       [&](flow::Graph const& graph)
                                       {
                                         ++depth;
                                         maxDepth = std::max(maxDepth, depth);
                                         graphs.push_back(graph);

                                         if (depth < 8U)
                                         {
                                           auto const volumeRes = backend.property(PropertyId::Volume);
                                           allReadsReturnedUnity =
                                             allReadsReturnedUnity && volumeRes && std::get<float>(*volumeRes) == 1.0F;
                                         }

                                         --depth;
                                       });

    CHECK(maxDepth == 1U);
    CHECK(allReadsReturnedUnity);
    REQUIRE(graphs.size() == 2U);
    REQUIRE(graphs.front().nodes.size() == 2U);
    REQUIRE(graphs.back().nodes.size() == 2U);
    CHECK(graphs.front().nodes.back().hardwareVolumeNotUnity);
    CHECK_FALSE(graphs.back().nodes.back().hardwareVolumeNotUnity);
    CHECK_FALSE(graphs.back().nodes.back().softwareVolumeNotUnity);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);
    CHECK(mixerStatePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaExclusiveBackend - retained publisher is inert after graph retirement",
            "[audio][unit][alsa][concurrency]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr));
    auto registry = detail::AlsaGraphRegistry{};
    auto graph = flow::Graph{};
    std::size_t graphUpdateCount = 0U;
    auto graphSub = registry.subscribe("hw:127,0",
                                       [&](flow::Graph const& nextGraph)
                                       {
                                         graph = nextGraph;
                                         ++graphUpdateCount;
                                       });
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};

    REQUIRE(backend.set(props::kVolume, 0.5F));
    REQUIRE(graphUpdateCount == 2U);
    REQUIRE_FALSE(graph.nodes.empty());

    registry.shutdown();
    REQUIRE(graphUpdateCount == 3U);
    CHECK(graph.nodes.empty());

    REQUIRE(backend.set(props::kVolume, 0.25F));
    backend.close();
    CHECK(graphUpdateCount == 3U);
    CHECK(graph.nodes.empty());
  }
} // namespace ao::audio::backend::test
