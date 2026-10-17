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

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

    auto const speakerMask = GENERATE(0U, 0x3U);
    auto const openedRes = backend.open(
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = speakerMask}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message.contains("direct hardware PCM"));
    // A native device that opened but is not a hardware PCM is an output-side
    // fault, never a constraint another track could satisfy.
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("AlsaExclusiveBackend - unsupported signal is rejected before native device inspection",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33};
    auto const openedRes = backend.open(sourceFormat, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::NotSupported);
    CHECK(openedRes.error().error.message == "No lossless PCM encoding is available for ALSA");
    // No lossless encoding can ever carry a 33-bit integer signal, so the
    // refusal follows the source, not this output.
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    // The same unrepresentable source must not even earn a decoder prewarm.
    CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
  }

  TEST_CASE(
    "AlsaExclusiveBackend - a float source without a 32-bit lossless encoding is refused before native inspection",
    "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // The null plugin opens successfully, so a regressed guard could only
    // surface the later non-hardware-PCM refusal, never this source refusal.
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const sourceFormat =
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint};

    auto const openedRes = backend.open(sourceFormat, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::NotSupported);
    CHECK(openedRes.error().error.message == "No lossless PCM encoding is available for ALSA");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
  }

  TEST_CASE("AlsaExclusiveBackend - zero geometry is refused as invalid input before native inspection",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // The null plugin opens successfully, so a regressed guard could only
    // surface the later non-hardware-PCM refusal, never this source refusal.
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "ALSA PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "ALSA PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("AlsaExclusiveBackend - geometry refusal outranks layout and precision refusals", "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    SECTION("a masked source with no sample rate still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "ALSA PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 33}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "ALSA PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - prewarm preserves identified and unidentified client modes without native calls",
            "[audio][unit][alsa]")
  {
    // The device is absent, so only immutable local admission policy can
    // answer; a native call could only fail, never produce a hint.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    std::uint32_t const sourceRate = 48000;
    std::uint8_t const sourceChannels = 2;

    SECTION("an identified source retains its canonical identities")
    {
      auto const optHint = backend.prewarmFormatHint(
        SignalFormat{.sampleRate = sourceRate, .channels = sourceChannels, .precisionBits = 24, .speakerMask = 0x3});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == sourceRate);
      CHECK(optHint->channels == sourceChannels);
      CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
      CHECK(optHint->speakerMask == 0x3U);
    }

    SECTION("the same source without identities keeps the ordinary hint")
    {
      auto const optHint = backend.prewarmFormatHint(
        SignalFormat{.sampleRate = sourceRate, .channels = sourceChannels, .precisionBits = 24});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == sourceRate);
      CHECK(optHint->channels == sourceChannels);
      CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
      CHECK(optHint->speakerMask == 0U);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - prewarm withholds deterministically invalid speaker declarations locally",
            "[audio][unit][alsa]")
  {
    // The withholding is decided by prewarmFormatHint's local admission
    // policy; the device plays no role in the decision.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    SECTION("an incomplete or reserved nonzero declaration is withheld")
    {
      auto const invalidMask = GENERATE(0x1U, 0x7U, 0x80000001U);
      CAPTURE(invalidMask);

      auto const optHint = backend.prewarmFormatHint(
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = invalidMask});

      CHECK_FALSE(optHint);
    }

    SECTION("a complete stereo declaration keeps the ordinary hint")
    {
      auto const optHint = backend.prewarmFormatHint(
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = 0x3});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000);
      CHECK(optHint->channels == 2);
      CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
      CHECK(optHint->speakerMask == 0x3U);
    }

    SECTION("an unidentified source keeps the ordinary hint")
    {
      auto const optHint =
        backend.prewarmFormatHint(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24});

      REQUIRE(optHint);
      CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
      CHECK(optHint->speakerMask == 0U);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - an invalid speaker declaration does not conceal a native open fault",
            "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // The declaration is deterministically unopenable, but open() must still
    // reach the device first: a missing card reports its own output fault
    // instead of a track refusal that hides the unavailable endpoint.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes =
      backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = 0x1}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code != Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message.contains("Failed to open ALSA device"));
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    // The same declaration stays locally withheld from prewarm as well.
    auto const optHint = backend.prewarmFormatHint(
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = 0x1});

    CHECK_FALSE(optHint);
  }

  TEST_CASE("AlsaExclusiveBackend - an identified source preserves genuine native open failure", "[audio][unit][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Position uncertainty must not conceal an independently failed native
    // open or turn an unavailable endpoint into a recoverable track refusal.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    // The channel count matches a normal stereo request, so only the assigned
    // speaker identities distinguish this source.
    auto const openedRes =
      backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = 0x3}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code != Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message.contains("Failed to open ALSA device"));
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
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
    CHECK(openedRes.error().error.code != Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message.contains("Failed to open ALSA device"));
    // An unavailable native device is a conservative output failure; it is
    // not evidence that the track itself is incompatible.
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
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
