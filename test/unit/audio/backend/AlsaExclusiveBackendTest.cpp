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

#include <cstddef>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - non-hardware PCM is rejected", "[audio][regression][alsa]")
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
            "[audio][regression][alsa]")
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
            "[audio][regression][alsa]")
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

  TEST_CASE("AlsaExclusiveBackend - a hint is never treated as an opened mode", "[audio][regression][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Nothing is cached before a successful open, so a device that was never
    // opened predicts only from immutable policy.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};

    std::ignore = backend.open(sourceFormat, target);

    auto const optHint = backend.prewarmFormatHint(sourceFormat);

    REQUIRE(optHint);
    CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
  }

  TEST_CASE("AlsaExclusiveBackend - NaN volume does not observe or publish mixer state", "[audio][regression][alsa]")
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
            "[audio][regression][alsa]")
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

  TEST_CASE("AlsaExclusiveBackend - graph callbacks can observe volume during publication and close",
            "[audio][regression][alsa][concurrency]")
  {
    auto mixerStatePtr = std::make_shared<detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    auto mixerFactory = detail::test::FakeMixerOpenFactory{mixerStatePtr};
    auto mixerPtr = std::make_unique<detail::AlsaMixerSession>(mixerFactory);
    REQUIRE(mixerPtr->tryInit(nullptr));
    auto registry = detail::AlsaGraphRegistry{};
    auto graph = flow::Graph{};
    auto const device = Device{
      .id = DeviceId{"hw:test,0"}, .displayName = "Test card", .description = "hw:test,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher(), std::move(mixerPtr)};
    bool reenter = false;
    bool observationSucceeded = false;
    float observedVolume = -1.0F;
    std::size_t reentryCount = 0U;
    auto graphSub = registry.subscribe("hw:test,0",
                                       [&](flow::Graph const& nextGraph)
                                       {
                                         graph = nextGraph;

                                         if (!reenter)
                                         {
                                           return;
                                         }

                                         reenter = false;
                                         ++reentryCount;
                                         auto const volumeRes = backend.property(PropertyId::Volume);
                                         observationSucceeded = volumeRes.has_value();

                                         if (volumeRes)
                                         {
                                           observedVolume = std::get<float>(*volumeRes);
                                         }
                                       });

    reenter = true;
    REQUIRE(backend.property(PropertyId::Volume));
    CHECK(observationSucceeded);
    CHECK(reentryCount == 1U);
    CHECK(observedVolume == 0.8F);
    CHECK_FALSE(graph.nodes.empty());

    reenter = true;
    backend.close();
    CHECK(observationSucceeded);
    CHECK(reentryCount == 2U);
    CHECK(observedVolume == 1.0F);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK(graph.nodes.empty());
  }

  TEST_CASE("AlsaExclusiveBackend - retained publisher is inert after graph retirement",
            "[audio][regression][alsa][concurrency]")
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
