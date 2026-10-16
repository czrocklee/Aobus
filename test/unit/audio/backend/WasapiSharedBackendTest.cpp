// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/WasapiSharedBackend.h"

#include "lib/audio/backend/WasapiProvider.h"
#include "lib/audio/backend/detail/WasapiError.h"
#include "lib/audio/backend/detail/WasapiFormat.h"
#include "lib/audio/backend/detail/WasapiGraphRegistry.h"
#include "lib/audio/backend/detail/WasapiRenderBuffer.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    Device testDevice()
    {
      return {.id = DeviceId{"endpoint-a"}, .displayName = "Test endpoint", .backendId = kBackendWasapi};
    }

    flow::Graph withDeclaredSource(flow::Graph graph, SignalFormat const& source)
    {
      graph.nodes.insert(graph.nodes.begin(),
                         flow::Node{.id = "ao-source",
                                    .type = flow::NodeType::Source,
                                    .optFormat = source,
                                    .sourceChannelLayout = {
                                      .origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = source.speakerMask}});
      graph.connections.push_back({.sourceId = "ao-source", .destinationId = "wasapi-stream", .isActive = true});
      return graph;
    }

    // An independently authored two-channel IEEE-float engine descriptor with
    // a complete FL/FR mask; each test mutates it for its own evidence case.
    WAVEFORMATEXTENSIBLE extensibleFloatStereoWave()
    {
      auto wave = WAVEFORMATEXTENSIBLE{};
      wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      wave.Format.nChannels = 2;
      wave.Format.nSamplesPerSec = 48000;
      wave.Format.wBitsPerSample = 32;
      wave.Format.nBlockAlign = 8;
      wave.Format.nAvgBytesPerSec = 384000;
      wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
      wave.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
      wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      wave.dwChannelMask = 3;
      return wave;
    }
  } // namespace

  TEST_CASE("WasapiSharedBackend - selected native endpoint observes stereo alternate and direct-out opens",
            "[audio][integration][wasapi][.manual]")
  {
    // Explicitly selected idle endpoint only. No start/render/PCM submission,
    // test setProperty call, endpoint-volume write or system-default change;
    // the canReusePcmMode probes are control-side reads of the live open.
    // Normal production open DOES restore cached volume/mute on this test
    // executable's own per-application session; this is not zero session writes.
    auto const* selectedId = std::getenv("AOBUS_TEST_WASAPI_DEVICE_ID");

    INFO("Set AOBUS_TEST_WASAPI_DEVICE_ID to an explicit endpoint ID or the test-only selector default");
    REQUIRE(selectedId != nullptr);
    REQUIRE(*selectedId != '\0');
    auto const selector = std::string{selectedId};
    auto provider = WasapiProvider{};
    auto const inventory = provider.status();
    // The sentinel reads the provider's existing default; it never changes the
    // Windows default or substitutes for an unmatched explicit endpoint ID.
    auto const matchesSelection = [&](Device const& candidate)
    { return selector == "default" ? candidate.isDefault : candidate.id.raw() == selector; };
    auto const matchingCount = std::ranges::count_if(inventory.devices, matchesSelection);
    WARN(std::format("Native WASAPI test selector={} matching-active-endpoints={}", selector, matchingCount));
    REQUIRE(matchingCount == 1);
    auto const selected = std::ranges::find_if(inventory.devices, matchesSelection);
    REQUIRE(selected != inventory.devices.end());
    auto const& device = *selected;
    WARN(std::format("Native WASAPI selected actual endpoint={} existing-default={} selector={}",
                     device.id.raw(),
                     device.isDefault,
                     selector));
    provider.shutdown();

    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto received = std::vector<flow::Graph>{};
    auto subscription =
      registryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { received.push_back(graph); });
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto backend = WasapiSharedBackend{device, kProfileShared, registryPtr};
    struct RequestCase final
    {
      std::uint8_t channels;
      std::uint32_t sourceMask;
      std::uint32_t nativeMask;
      ChannelDeliveryEvidence::Mapping mapping;
      bool requiresSuccess;
    };
    auto const requests =
      std::array{RequestCase{2, 0x3, 0x3, ChannelDeliveryEvidence::Mapping::Mapped, true},
                 RequestCase{2, 0, 0x3, ChannelDeliveryEvidence::Mapping::LegacyUnverified, true},
                 RequestCase{2, 0x30, 0x30, ChannelDeliveryEvidence::Mapping::Mapped, false},
                 RequestCase{6, 0x3f, 0, ChannelDeliveryEvidence::Mapping::LegacyUnverified, false}};

    for (auto const& request : requests)
    {
      auto const source = SignalFormat{
        .sampleRate = 48000, .channels = request.channels, .precisionBits = 16, .speakerMask = request.sourceMask};
      auto const openedRes = backend.open(source, target);

      if (!openedRes)
      {
        auto const& failure = openedRes.error();
        WARN(std::format("Native WASAPI endpoint={} request=48000Hz/{}ch/16bit/source-mask={:#x}/descriptor-mask={:#x} "
                         "result=refused cause={} code={} diagnostic={}; no active mix graph published",
                         device.id.raw(),
                         request.channels,
                         request.sourceMask,
                         request.nativeMask,
                         failure.cause == BackendOpenFailure::Cause::TrackRouteIncompatible ? "TrackRouteIncompatible"
                                                                                            : "OutputFailure",
                         static_cast<std::uint32_t>(failure.error.code),
                         failure.error.message));
        REQUIRE_FALSE(request.requiresSuccess);
        // All requests passed local source admission. Only the native
        // Initialize unsupported-format category is a valid refusal control;
        // busy/lost devices and unclassified faults fail, never skip this case.
        REQUIRE(failure.cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
        CHECK(failure.error.code == Error::Code::FormatRejected);
        CHECK_FALSE(failure.error.message.empty());
        backend.close();
        continue;
      }

      CHECK(openedRes->clientFormat.sampleRate == 48000U);
      CHECK(openedRes->clientFormat.channels == request.channels);
      CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(openedRes->clientFormat.speakerMask == request.sourceMask);
      CHECK_FALSE(openedRes->optEndpoint);
      CHECK(openedRes->channelDelivery.mapping == request.mapping);
      CHECK(openedRes->channelDelivery.nativeSpeakerMask == request.nativeMask);
      REQUIRE_FALSE(received.empty());
      auto const& graph = received.back();
      auto const stream = std::ranges::find(graph.nodes, "wasapi-stream", &flow::Node::id);
      auto const sink = std::ranges::find(graph.nodes, "wasapi-sink", &flow::Node::id);
      REQUIRE(stream != graph.nodes.end());
      REQUIRE(sink != graph.nodes.end());
      REQUIRE(stream->optFormat);
      auto const* client = std::get_if<PcmFormat>(&*stream->optFormat);
      REQUIRE(client != nullptr);
      CHECK(client->channels == request.channels);
      CHECK(client->speakerMask == request.nativeMask);
      CHECK(stream->channelDelivery.mapping == openedRes->channelDelivery.mapping);
      CHECK(stream->channelDelivery.nativeSpeakerMask == openedRes->channelDelivery.nativeSpeakerMask);
      CHECK_FALSE(stream->channelDelivery.hasSharedConversion);

      // This observation campaign requires a supported actual engine mix
      // representation. An unavailable observation is not fabricated from the
      // client, nor accepted as native mix-format evidence for this case.
      REQUIRE(sink->optFormat);
      auto const* mix = std::get_if<PcmFormat>(&*sink->optFormat);
      REQUIRE(mix != nullptr);
      REQUIRE(mix->channels > 0U);
      REQUIRE(mix->sampleRate > 0U);
      auto const nativeMixMask = sink->channelDelivery.nativeSpeakerMask;
      CHECK(sink->channelDelivery.mapping == (mix->speakerMask != 0U
                                                ? ChannelDeliveryEvidence::Mapping::Mapped
                                                : ChannelDeliveryEvidence::Mapping::LegacyUnverified));

      if (mix->speakerMask != 0U)
      {
        CHECK(nativeMixMask == mix->speakerMask);
      }
      else
      {
        CHECK(std::popcount(nativeMixMask) < mix->channels);
      }

      // Literal unit cases below own conversion-policy oracles. This dynamic
      // native control checks API/dataflow consistency, not independent policy
      // correctness or physical speaker delivery.
      bool const knownConversion =
        mix->channels != request.channels || (request.nativeMask != 0U && nativeMixMask != 0U &&
                                              std::popcount(request.nativeMask | nativeMixMask) > mix->channels);
      CHECK(openedRes->channelDelivery.hasSharedConversion == knownConversion);
      CHECK(sink->channelDelivery.hasSharedConversion == knownConversion);
      WARN(std::format("Native WASAPI endpoint={} request=48000Hz/{}ch/16bit/source-mask={:#x} result=opened "
                       "Initialize=success cause=none diagnostic=production-open-success canonical-client-mask={:#x} "
                       "accepted-descriptor-mask={:#x} mapping={} independent-engine-mix={}Hz/{}ch/{}/mask={:#x} "
                       "engine-native-set={:#x} engine-mapping={} shared-conversion={}; no physical/capture proof",
                       device.id.raw(),
                       request.channels,
                       request.sourceMask,
                       openedRes->clientFormat.speakerMask,
                       client->speakerMask,
                       static_cast<std::uint32_t>(openedRes->channelDelivery.mapping),
                       mix->sampleRate,
                       mix->channels,
                       sampleEncodingName(mix->encoding),
                       mix->speakerMask,
                       nativeMixMask,
                       static_cast<std::uint32_t>(sink->channelDelivery.mapping),
                       openedRes->channelDelivery.hasSharedConversion));

      // Open-only reuse proof: the live initialized client descriptor decides,
      // never GetMixFormat, the render transport or graph evidence. Both stereo
      // identities open through the same accepted FL/FR descriptor, so only
      // that base proves the 0 <-> FL/FR identity change; alternate sets and
      // geometry changes never do.
      auto const stereoFlip = PcmFormat{.sampleRate = 48000,
                                        .channels = 2,
                                        .encoding = SampleEncoding::Signed16Le,
                                        .speakerMask = request.sourceMask == 0x3U ? 0U : 0x3U};
      auto const centeredSet =
        PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x5};
      auto const otherRate = PcmFormat{
        .sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = request.sourceMask};
      CHECK(backend.canReusePcmMode(openedRes->clientFormat, openedRes->clientFormat));
      CHECK(backend.canReusePcmMode(openedRes->clientFormat, stereoFlip) == (request.nativeMask == 0x3U));
      CHECK(backend.canReusePcmMode(stereoFlip, openedRes->clientFormat) == (request.nativeMask == 0x3U));
      CHECK_FALSE(backend.canReusePcmMode(openedRes->clientFormat, centeredSet));
      CHECK_FALSE(backend.canReusePcmMode(openedRes->clientFormat, otherRate));

      backend.close();
      // The closed stream proves nothing, not even the identity change that
      // the same descriptor proved while its initialized client was live.
      CHECK_FALSE(backend.canReusePcmMode(openedRes->clientFormat, stereoFlip));
      REQUIRE_FALSE(received.empty());
      CHECK(received.back().nodes.empty());
    }
  }

  TEST_CASE("WasapiSharedBackend - identity and unopened lifecycle are stable", "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    CHECK(backend.backendId() == kBackendWasapi);
    CHECK(backend.profileId() == kProfileShared);

    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();
  }

  TEST_CASE("WasapiSharedBackend - mode reuse without a live initialized client stays exact",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    auto const unknownStereo =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0};
    auto const frontStereo =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const backStereo =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x30};

    // Exact equality stays the default admission without an initialized client.
    CHECK(backend.canReusePcmMode(unknownStereo, unknownStereo));
    CHECK(backend.canReusePcmMode(frontStereo, frontStereo));
    CHECK(backend.canReusePcmMode(backStereo, backStereo));

    // No native client is live, so the 0 <-> FL/FR identity change stays
    // unproved even though both identities share one accepted descriptor.
    CHECK_FALSE(backend.canReusePcmMode(frontStereo, unknownStereo));
    CHECK_FALSE(backend.canReusePcmMode(unknownStereo, frontStereo));

    // Geometry and storage changes never reuse.
    CHECK_FALSE(backend.canReusePcmMode(
      frontStereo,
      PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3}));
    CHECK_FALSE(backend.canReusePcmMode(
      frontStereo,
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed32Le, .speakerMask = 0x3}));
    CHECK_FALSE(backend.canReusePcmMode(
      frontStereo,
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x4}));

    // Alternate two-role sets are never the reuse base.
    CHECK_FALSE(backend.canReusePcmMode(backStereo, frontStereo));
    CHECK_FALSE(backend.canReusePcmMode(frontStereo, backStereo));

    // Closing without an open proves nothing either.
    backend.close();
    CHECK_FALSE(backend.canReusePcmMode(frontStereo, unknownStereo));
  }

  TEST_CASE("WasapiSharedBackend - cached properties round trip before opening a device",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, 0.25F));
    REQUIRE(backend.set(props::kMuted, true));
    auto const volumeRes = backend.get(props::kVolume);
    auto const mutedRes = backend.get(props::kMuted);

    REQUIRE(volumeRes);
    REQUIRE(mutedRes);
    CHECK(*volumeRes == 0.25F);
    CHECK(*mutedRes);

    auto const volumeInfo = backend.queryProperty(PropertyId::Volume);
    auto const mutedInfo = backend.queryProperty(PropertyId::Muted);
    CHECK(volumeInfo.canRead);
    CHECK(volumeInfo.canWrite);
    CHECK_FALSE(volumeInfo.isAvailable);
    CHECK_FALSE(volumeInfo.emitsChangeNotifications);
    CHECK_FALSE(volumeInfo.isHardwareAssisted);
    CHECK(mutedInfo == volumeInfo);
  }

  TEST_CASE("WasapiSharedBackend - unopened property intent does not claim applied session state",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto received = std::vector<flow::Graph>{};
    auto sub = registryPtr->subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, -0.5F));
    auto volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 0.0F);
    REQUIRE(received.size() == 2);
    REQUIRE(received.back().nodes.size() == 2);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    auto const unappliedGraph = received.back();
    CHECK(received.front() == unappliedGraph);
    REQUIRE(backend.set(props::kVolume, 1.5F));
    volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 1.0F);
    // Changed unopened intent leaves the applied-session graph unchanged, so it is not redelivered.
    REQUIRE(received.size() == 2);
    CHECK(received.back() == unappliedGraph);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    backend.close();
    REQUIRE(received.size() == 3);
    CHECK(received.back().nodes.empty());
    CHECK(received.back().connections.empty());
  }

  TEST_CASE("WasapiSharedBackend - a partial speaker set is rejected before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    // One assigned role cannot describe this two-channel source.
    auto const openedRes =
      backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x1}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::NotSupported);
    CHECK(openedRes.error().error.message == "WASAPI requires a complete supported speaker set");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE(
    "WasapiSharedBackend - an unrepresentable source is refused as a track incompatibility before endpoint access",
    "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000,
                                             .channels = 2,
                                             .precisionBits = 33,
                                             .sampleKind = SampleKind::Integer,
                                             .speakerMask = 0x30};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint},
        target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("WasapiSharedBackend - zero geometry is refused as invalid input before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("WasapiSharedBackend - geometry refusal outranks layout and precision refusals",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a masked source with no sample rate still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 33}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("WasapiSharedBackend - prewarm preserves complete admitted sets without endpoint negotiation",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    for (auto const source :
         {SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x30},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x5},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x1},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x4},
          SignalFormat{.sampleRate = 48000, .channels = 4, .precisionBits = 16, .speakerMask = 0x33}})
    {
      auto const optHint = backend.prewarmFormatHint(source);
      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == source.channels);
      CHECK(optHint->speakerMask == source.speakerMask);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }

    CHECK_FALSE(backend.prewarmFormatHint(
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 1}));
  }

  TEST_CASE("prepareWasapiRenderPacket pads short PCM without advancing silence", "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    auto const packet = detail::prepareWasapiRenderPacket(
      buffer, 4, {.bytesWritten = 8, .positionFrameOffset = 1, .positionFrames = 1, .drained = false});

    CHECK(packet.renderedFrames == 2);
    CHECK(packet.framesToRelease == 4);
    CHECK(packet.positionFrames == 1);
    CHECK(packet.underrun);
    CHECK_FALSE(packet.drained);

    for (std::size_t index = 0; index < buffer.size(); ++index)
    {
      CHECK(buffer[index] == (index < 8 ? std::byte{0x5a} : std::byte{0}));
    }
  }

  TEST_CASE("prepareWasapiRenderPacket releases drained short and empty packets correctly",
            "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    SECTION("a final PCM prefix commits a complete silent-padded packet")
    {
      auto const packet = detail::prepareWasapiRenderPacket(buffer, 4, {.bytesWritten = 4, .drained = true});

      CHECK(packet.renderedFrames == 1);
      CHECK(packet.framesToRelease == 4);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.front() == std::byte{0x5a});
      CHECK(buffer.back() == std::byte{0});
    }

    SECTION("an empty drained packet is released without a commit")
    {
      auto const packet = detail::prepareWasapiRenderPacket(buffer, 4, {.bytesWritten = 0, .drained = true});

      CHECK(packet.renderedFrames == 0);
      CHECK(packet.framesToRelease == 0);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.back() == std::byte{0x5a});
    }
  }

  TEST_CASE("observeWaveFormat preserves endpoint mix precision", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 8;
    wave.Format.nAvgBytesPerSec = 384000;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    wave.Samples.wValidBitsPerSample = 24; // NOLINT(cppcoreguidelines-pro-type-union-access)
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    auto const observationRes = detail::observeWaveFormat(wave.Format);

    REQUIRE(observationRes);
    REQUIRE(observationRes->optFormat);
    CHECK(observationRes->optFormat->sampleRate == 48000);
    CHECK(observationRes->optFormat->channels == 2);
    CHECK(observationRes->optFormat->encoding == SampleEncoding::Signed24In32Le);
  }

  TEST_CASE("WasapiFormat - actual descriptors preserve alternate small sets and legacy direct-out",
            "[audio][unit][wasapi][format]")
  {
    struct DescriptorCase final
    {
      std::uint8_t channels;
      std::uint32_t sourceMask;
      std::uint32_t nativeMask;
      ChannelDeliveryEvidence::Mapping mapping;
    };
    using Mapping = ChannelDeliveryEvidence::Mapping;
    auto const cases = std::array{DescriptorCase{1, 0x1, 0x1, Mapping::Mapped},
                                  DescriptorCase{1, 0x4, 0x4, Mapping::Mapped},
                                  DescriptorCase{2, 0x3, 0x3, Mapping::Mapped},
                                  DescriptorCase{2, 0x30, 0x30, Mapping::Mapped},
                                  DescriptorCase{2, 0x5, 0x5, Mapping::Mapped},
                                  DescriptorCase{1, 0, 0x4, Mapping::LegacyUnverified},
                                  DescriptorCase{2, 0, 0x3, Mapping::LegacyUnverified},
                                  DescriptorCase{4, 0x33, 0, Mapping::LegacyUnverified},
                                  DescriptorCase{6, 0x3f, 0, Mapping::LegacyUnverified}};

    for (auto const& fixture : cases)
    {
      CAPTURE(fixture.sourceMask, fixture.channels);
      auto const application = PcmFormat{.sampleRate = 48000,
                                         .channels = fixture.channels,
                                         .encoding = SampleEncoding::Signed16Le,
                                         .speakerMask = fixture.sourceMask};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      CHECK(optWave->dwChannelMask == fixture.nativeMask);
      CHECK(optWave->Format.nChannels == fixture.channels);
      CHECK(optWave->Format.nBlockAlign == fixture.channels * 2U);
      auto const observationRes = detail::observeWaveFormat(optWave->Format);
      REQUIRE(observationRes);
      REQUIRE(observationRes->optFormat);
      CHECK(observationRes->optFormat->speakerMask == fixture.nativeMask);
      CHECK(observationRes->optFormat->encoding == SampleEncoding::Signed16Le);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, {});
      CHECK(evidence.mapping == fixture.mapping);
      CHECK(evidence.nativeSpeakerMask == fixture.nativeMask);
      CHECK_FALSE(evidence.hasSharedConversion);
      CHECK(application.speakerMask == fixture.sourceMask);
    }
  }

  TEST_CASE("WasapiFormat - parses independent engine masks and keeps conversion separate from mapping",
            "[audio][unit][wasapi][format]")
  {
    // This is an independently authored engine descriptor, not a copy of the
    // source request. These model tests do not execute Initialize/GetMixFormat.
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 0x3;
    auto const mixRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    CHECK(mixRes->speakerMask == 0x3U);
    CHECK(mixRes->optFormat->speakerMask == 0x3U);

    SECTION("alternate same-count set is a known shared conversion with mapped client roles")
    {
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x30};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *mixRes);
      CHECK(evidence.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 0x30U);
    }

    SECTION("direct-out count conversion cannot erase legacy uncertainty")
    {
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *mixRes);
      CHECK(evidence.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 0U);
    }

    SECTION("accepted centered mono can have independently observed FL-only engine conversion")
    {
      mix.Format.nChannels = 1;
      mix.Format.nBlockAlign = 4;
      mix.Format.nAvgBytesPerSec = 192000;
      mix.dwChannelMask = 1;
      auto const monoMixRes = detail::observeWaveFormat(mix.Format);
      REQUIRE(monoMixRes);
      REQUIRE(monoMixRes->optFormat);
      CHECK(monoMixRes->optFormat->speakerMask == 1U);
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 4};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *monoMixRes);
      CHECK(evidence.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 4U);
    }

    SECTION("unknown engine mask remains unknown independently of the client request")
    {
      mix.dwChannelMask = 0;
      auto const unknownRes = detail::observeWaveFormat(mix.Format);
      REQUIRE(unknownRes);
      REQUIRE(unknownRes->optFormat);
      CHECK(unknownRes->speakerMask == 0U);
      CHECK(unknownRes->optFormat->speakerMask == 0U);
    }
  }

  TEST_CASE("WasapiFormat - unsafe descriptors cannot be constructed", "[audio][unit][wasapi][format]")
  {
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 0, .channels = 2, .encoding = SampleEncoding::Signed16Le}));
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 48000, .channels = 0, .encoding = SampleEncoding::Signed16Le}));
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Unknown}));
    CHECK_FALSE(detail::waveFormatFromPcmFormat(
      {.sampleRate = 0xffffffff, .channels = 2, .encoding = SampleEncoding::Signed32Le}));
    CHECK_FALSE(detail::waveFormatFromPcmFormat(
      {.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 1}));
  }

  TEST_CASE("WasapiSharedBackend - both stereo identities accept one shared FL FR client descriptor",
            "[audio][unit][wasapi][format]")
  {
    auto const stereoOf = [](std::uint32_t const speakerMask)
    {
      return PcmFormat{
        .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = speakerMask};
    };
    auto const optUnknownWave = detail::waveFormatFromPcmFormat(stereoOf(0));
    auto const optFrontWave = detail::waveFormatFromPcmFormat(stereoOf(0x3));
    REQUIRE(optUnknownWave);
    REQUIRE(optFrontWave);

    // WAVEFORMATEXTENSIBLE has no equality operator, so the literal factory
    // oracle compares field by field: an unknown-stereo source and a declared
    // FL/FR source are accepted through one identical descriptor, which is
    // the fixed base any later stereo-reuse proof is bound to.
    CHECK(optUnknownWave->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    CHECK(optFrontWave->Format.wFormatTag == optUnknownWave->Format.wFormatTag);
    CHECK(optUnknownWave->Format.nChannels == 2);
    CHECK(optFrontWave->Format.nChannels == optUnknownWave->Format.nChannels);
    CHECK(optUnknownWave->Format.nSamplesPerSec == 48000);
    CHECK(optFrontWave->Format.nSamplesPerSec == optUnknownWave->Format.nSamplesPerSec);
    CHECK(optUnknownWave->Format.wBitsPerSample == 16);
    CHECK(optFrontWave->Format.wBitsPerSample == optUnknownWave->Format.wBitsPerSample);
    CHECK(optUnknownWave->Format.nBlockAlign == 4);
    CHECK(optFrontWave->Format.nBlockAlign == optUnknownWave->Format.nBlockAlign);
    CHECK(optUnknownWave->Format.nAvgBytesPerSec == 192000);
    CHECK(optFrontWave->Format.nAvgBytesPerSec == optUnknownWave->Format.nAvgBytesPerSec);
    CHECK(optFrontWave->Format.cbSize == optUnknownWave->Format.cbSize);
    CHECK(optUnknownWave->dwChannelMask == 0x3U);
    CHECK(optFrontWave->dwChannelMask == optUnknownWave->dwChannelMask);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    CHECK(optUnknownWave->Samples.wValidBitsPerSample == 16); // NOLINT(cppcoreguidelines-pro-type-union-access)
    CHECK(optFrontWave->Samples.wValidBitsPerSample ==        // NOLINT(cppcoreguidelines-pro-type-union-access)
          optUnknownWave->Samples.wValidBitsPerSample);
    CHECK(::IsEqualGUID(optFrontWave->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE);
    CHECK(::IsEqualGUID(optFrontWave->SubFormat, optUnknownWave->SubFormat) != FALSE);

    SECTION("alternate two-role sets build their own descriptors without the FL FR base")
    {
      auto const optBackWave = detail::waveFormatFromPcmFormat(stereoOf(0x30));
      REQUIRE(optBackWave);
      CHECK(optBackWave->dwChannelMask == 0x30U);
      CHECK(optBackWave->Format.nBlockAlign == optUnknownWave->Format.nBlockAlign);

      auto const optCenteredWave = detail::waveFormatFromPcmFormat(stereoOf(0x5));
      REQUIRE(optCenteredWave);
      CHECK(optCenteredWave->dwChannelMask == 0x5U);
    }

    SECTION("geometry and storage changes build different descriptors")
    {
      auto const optOtherRateWave = detail::waveFormatFromPcmFormat(
        PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3});
      REQUIRE(optOtherRateWave);
      CHECK(optOtherRateWave->Format.nSamplesPerSec == 44100);
      CHECK(optOtherRateWave->Format.nAvgBytesPerSec == 176400);

      auto const optWideWave = detail::waveFormatFromPcmFormat(
        PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed32Le, .speakerMask = 0x3});
      REQUIRE(optWideWave);
      CHECK(optWideWave->Format.wBitsPerSample == 32);
      CHECK(optWideWave->Format.nBlockAlign == 8);
      CHECK(optWideWave->Format.nAvgBytesPerSec == 384000);

      auto const optMonoWave = detail::waveFormatFromPcmFormat(
        PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x1});
      REQUIRE(optMonoWave);
      CHECK(optMonoWave->Format.nChannels == 1);
      CHECK(optMonoWave->dwChannelMask == 0x1U);
    }
  }

  TEST_CASE("WasapiGraphRegistry - client to mix conversion belongs only to its sink",
            "[audio][integration][wasapi][graph]")
  {
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples = {.wValidBitsPerSample = 32};
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 3;
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f};
    auto const optClientWave = detail::waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = detail::observeWaveFormat(optClientWave->Format);
    auto const mixRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optClientWave, *mixRes);
    CHECK(evidence.hasSharedConversion);
    auto source = signalFormat(application);
    bool hasUpstreamConversion = false;

    SECTION("a six to two channel conversion produces only one sink finding")
    {
    }

    SECTION("a distinct four to six channel transition still produces its own stream finding")
    {
      source.channels = 4;
      source.speakerMask = 0x33;
      hasUpstreamConversion = true;
    }

    auto registry = detail::WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .optMixFormat = mixRes->optFormat,
                      .mixChannels = mixRes->channels,
                      .mixSpeakerMask = mixRes->speakerMask,
                      .channelDelivery = evidence});
    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    REQUIRE(stream.optFormat);
    REQUIRE(sink.optFormat);
    CHECK(signalFormat(*stream.optFormat).speakerMask == 0U);
    CHECK(signalFormat(*sink.optFormat).speakerMask == 3U);
    CHECK(stream.channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(sink.channelDelivery.nativeSpeakerMask == 3U);
    CHECK(sink.channelDelivery.hasSharedConversion);

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), source));
    CHECK(analysis.channelConfidence == ChannelConfidence::Converted);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    CHECK(analysis.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(analysis.assessments.size() == 3U);
    auto const& streamAssessment = analysis.assessments[1];
    auto const& sinkAssessment = analysis.assessments[2];
    CHECK(streamAssessment.nodeId == "wasapi-stream");
    CHECK(sinkAssessment.nodeId == "wasapi-sink");
    CHECK(std::ranges::count(streamAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) ==
          (hasUpstreamConversion ? 1 : 0));
    CHECK(std::ranges::count(sinkAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 1);
    auto const conversion =
      std::ranges::find(sinkAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
    REQUIRE(conversion != sinkAssessment.findings.end());
    REQUIRE(conversion->optFromFormat);
    REQUIRE(conversion->optToFormat);
    CHECK(signalFormat(*conversion->optFromFormat).channels == 6U);
    CHECK(signalFormat(*conversion->optToFormat).channels == 2U);
  }

  TEST_CASE("WasapiGraphRegistry - parsed engine positions establish only the software sink boundary",
            "[audio][integration][wasapi][graph]")
  {
    // Independent engine descriptor and accepted-client descriptor models;
    // this test executes neither GetMixFormat nor Initialize.
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 3;
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};
    auto const optClientWave = detail::waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = detail::observeWaveFormat(optClientWave->Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    auto expectedMapping = ChannelDeliveryEvidence::Mapping::Mapped;
    std::uint32_t expectedMixMask = 3;
    std::uint32_t expectedFormatMask = 3;
    bool hasMixFormat = true;
    bool hasSharedConversion = false;
    auto expectedConfidence = ChannelConfidence::Verified;

    SECTION("matching complete engine positions establish mapping without a conversion")
    {
      mix.dwChannelMask = 3;
    }

    SECTION("excess high assigned positions normalize two channel mask seven to FL FR")
    {
      mix.dwChannelMask = 0x7;
    }

    SECTION("partial FL is compatible uncertainty without a canonical complete format mask")
    {
      mix.dwChannelMask = 0x1;
      expectedMixMask = 0x1;
      expectedFormatMask = 0;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("partial FC contradicts the FL FR client without being promoted to full mapping")
    {
      mix.dwChannelMask = 0x4;
      expectedMixMask = 0x4;
      expectedFormatMask = 0;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      hasSharedConversion = true;
      expectedConfidence = ChannelConfidence::Converted;
    }

    SECTION("an independently observed alternate set is mapped and converted")
    {
      mix.dwChannelMask = 0x30;
      expectedMixMask = 0x30;
      expectedFormatMask = 0x30;
      hasSharedConversion = true;
      expectedConfidence = ChannelConfidence::Converted;
    }

    SECTION("an observed zero mask leaves sink mapping unverified")
    {
      mix.dwChannelMask = 0;
      expectedMixMask = 0;
      expectedFormatMask = 0;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("a known count conversion coexists with unknown sink positions")
    {
      mix.dwChannelMask = 0;
      mix.Format.nChannels = 4;
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      expectedMixMask = 0;
      expectedFormatMask = 0;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      hasSharedConversion = true;
      expectedConfidence = ChannelConfidence::Converted;
    }

    SECTION("unsupported float width retains actual count and role observations")
    {
      mix.Format.wBitsPerSample = 64;
      mix.Samples.wValidBitsPerSample = 64; // NOLINT(cppcoreguidelines-pro-type-union-access)
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      mix.dwChannelMask = 0x30;
      expectedMixMask = 0x30;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Converted;
    }

    SECTION("unsupported subtype retains independent multichannel mix positions")
    {
      mix.SubFormat = {};
      mix.Format.nChannels = 6;
      mix.Format.nBlockAlign = 24;
      mix.Format.nAvgBytesPerSec = 1152000;
      mix.dwChannelMask = 0x3f;
      expectedMixMask = 0x3f;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Converted;
    }

    SECTION("unsupported float width still retains compatible partial roles")
    {
      mix.Format.wBitsPerSample = 64;
      mix.Samples = {.wValidBitsPerSample = 64};
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      mix.dwChannelMask = 0x1;
      expectedMixMask = 0x1;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("unsupported subtype still retains a conflicting partial FC constraint")
    {
      mix.SubFormat = {};
      mix.dwChannelMask = 0x4;
      expectedMixMask = 0x4;
      expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Converted;
    }

    auto const observationRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(observationRes);
    REQUIRE(observationRes->optFormat.has_value() == hasMixFormat);
    CHECK(observationRes->channels == mix.Format.nChannels);
    CHECK(observationRes->speakerMask == expectedMixMask);
    auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optClientWave, *observationRes);
    CHECK(evidence.hasSharedConversion == hasSharedConversion);
    auto registry = detail::WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .optMixFormat = observationRes->optFormat,
                      .mixChannels = observationRes->channels,
                      .mixSpeakerMask = observationRes->speakerMask,
                      .channelDelivery = evidence});

    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    CHECK(stream.channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == 3U);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.channelDelivery.mapping == expectedMapping);
    CHECK(sink.channelDelivery.nativeSpeakerMask == expectedMixMask);
    CHECK(sink.channelDelivery.hasSharedConversion == hasSharedConversion);
    REQUIRE(stream.optFormat);
    CHECK(signalFormat(*stream.optFormat).speakerMask == 3U);
    REQUIRE(sink.optFormat.has_value() == hasMixFormat);

    if (sink.optFormat)
    {
      CHECK(signalFormat(*sink.optFormat).speakerMask == expectedFormatMask);
    }

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), signalFormat(application)));
    CHECK(analysis.channelConfidence == expectedConfidence);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    CHECK(analysis.fullyVerified == hasMixFormat);
    CHECK(analysis.pipelineQuality == (hasSharedConversion ? Quality::LinearIntervention
                                       : hasMixFormat      ? Quality::LosslessFloat
                                                           : Quality::BitwisePerfect));
    REQUIRE(analysis.assessments.size() == 3U);
    CHECK(std::ranges::count(
            analysis.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    CHECK(std::ranges::count(analysis.assessments[2].findings,
                             QualityFindingKind::ChannelMapping,
                             &QualityFinding::kind) == (hasSharedConversion ? 1 : 0));
    CHECK(analysis.assessments[2].channelDelivery.nativeSpeakerMask == expectedMixMask);

    if (hasSharedConversion && hasMixFormat)
    {
      auto const& findings = analysis.assessments[2].findings;
      auto const conversion = std::ranges::find(findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
      REQUIRE(conversion != findings.end());
      REQUIRE(conversion->optFromFormat);
      REQUIRE(conversion->optToFormat);
      CHECK(signalFormat(*conversion->optFromFormat).speakerMask == 0x3U);
      CHECK(conversion->optFromSpeakerMask.value_or(signalFormat(*conversion->optFromFormat).speakerMask) == 0x3U);
      // Complete sets may already be carried by the actual format mask.
      CHECK(conversion->optToSpeakerMask.value_or(signalFormat(*conversion->optToFormat).speakerMask) ==
            expectedMixMask);

      if (expectedMixMask == 0x4U)
      {
        REQUIRE(conversion->optToSpeakerMask);
        CHECK(*conversion->optToSpeakerMask == 0x4U);
        CHECK(signalFormat(*conversion->optToFormat).speakerMask == 0U);
      }
    }
  }

  TEST_CASE("WasapiFormat - a short extensible descriptor is a storage fault that absent numerics cannot hide",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();

    SECTION("a short successful extensible descriptor is refused before any extensible read")
    {
      wave.Format.cbSize = 0;
    }

    SECTION("zero dimensions cannot hide the short extensible object")
    {
      wave.Format.cbSize = 0;
      wave.Format.nSamplesPerSec = 0;
      wave.Format.nChannels = 0;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == Error::Code::FormatRejected);
    CHECK_FALSE(observationRes.error().message.empty());
  }

  TEST_CASE("WasapiFormat - absent mix numerics are an unavailable observation without guessed geometry",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();

    SECTION("zero native count")
    {
      wave.Format.nChannels = 0;
    }

    SECTION("zero native rate")
    {
      wave.Format.nSamplesPerSec = 0;
    }

    SECTION("zero count and rate keep a complete mask unusable")
    {
      wave.Format.nChannels = 0;
      wave.Format.nSamplesPerSec = 0;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 0U);
    CHECK(observationRes->channels == 0U);
    CHECK(observationRes->speakerMask == 0U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("WasapiFormat - contradictory mix arithmetic withholds the concrete format but keeps facts",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();
    std::uint32_t expectedMask = 3;

    SECTION("zero native stride")
    {
      wave.Format.nBlockAlign = 0;
    }

    SECTION("zero native byte rate")
    {
      wave.Format.nAvgBytesPerSec = 0;
    }

    SECTION("inconsistent native stride")
    {
      wave.Format.nBlockAlign = 4;
    }

    SECTION("inconsistent native byte rate")
    {
      wave.Format.nAvgBytesPerSec = 192000;
    }

    SECTION("a legal partial mask cannot hide inconsistent native stride")
    {
      wave.dwChannelMask = 1;
      wave.Format.nBlockAlign = 4;
      expectedMask = 1;
    }

    SECTION("a legal excess mask cannot hide inconsistent native byte rate")
    {
      wave.dwChannelMask = 7;
      wave.Format.nAvgBytesPerSec = 192000;
      expectedMask = 3;
    }

    SECTION("precision exceeds the container")
    {
      wave.Samples.wValidBitsPerSample = 33; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == expectedMask);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("WasapiFormat - reserved mix positions erase the role set instead of becoming legal",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();
    bool hasConcreteFormat = true;

    SECTION("reserved positions remain outside supported native mask policy")
    {
      wave.dwChannelMask = 0x40000;
    }

    SECTION("wildcard positions remain outside supported native mask policy")
    {
      wave.dwChannelMask = 0x80000001;
    }

    SECTION("a reserved position alongside a complete set erases the whole role set")
    {
      wave.dwChannelMask = 0x40003;
    }

    SECTION("unsupported subtype cannot hide a reserved native mask")
    {
      wave.SubFormat = {};
      wave.dwChannelMask = 0x80000001;
      hasConcreteFormat = false;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    // Reserved or wildcard positions are never declared legal or natively
    // mapped: the whole role set is erased and the canonical mask stays 0.
    CHECK(observationRes->speakerMask == 0U);
    CHECK(observationRes->optFormat.has_value() == hasConcreteFormat);

    if (observationRes->optFormat)
    {
      CHECK(observationRes->optFormat->sampleRate == 48000U);
      CHECK(observationRes->optFormat->channels == 2U);
      CHECK(observationRes->optFormat->encoding == SampleEncoding::Float32Le);
      CHECK(observationRes->optFormat->speakerMask == 0U);
    }
  }

  TEST_CASE("WasapiGraphRegistry - incomplete mix sets never promote sink mapping or erase conversion",
            "[audio][unit][wasapi][graph]")
  {
    auto registry = detail::WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });

    for (auto const mask : {std::uint32_t{0}, std::uint32_t{1}})
    {
      CAPTURE(mask);
      registry.publish(
        {.routeAnchor = "endpoint-a",
         .optMixFormat =
           PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Float32Le, .speakerMask = 0},
         .mixChannels = 2,
         .mixSpeakerMask = mask,
         .channelDelivery = {.hasSharedConversion = true}});
      REQUIRE(received.back().nodes.size() == 2U);
      auto const& sink = received.back().nodes[1];
      CHECK(sink.channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(sink.channelDelivery.nativeSpeakerMask == mask);
      CHECK(sink.channelDelivery.hasSharedConversion);
    }
  }

  TEST_CASE("WasapiGraphRegistry - unusable mix evidence never fabricates a sink format, layout or conflict",
            "[audio][integration][wasapi][graph]")
  {
    // Parser -> graph -> analyzer regression. The engine's optional mix
    // observation contributes independently usable facts only: contradictory
    // or absent numeric evidence must not become a fabricated sink PCM format
    // or Verified delivery, and reserved positions must not become a layout
    // or a false channel conflict.
    auto mix = extensibleFloatStereoWave();
    auto application =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    std::uint32_t expectedMixRate = 48000;
    std::uint16_t expectedMixChannels = 2;
    std::uint32_t expectedMixMask = 0;
    bool mixHasFormat = false;
    bool sinkHasFormat = false;
    auto expectedSinkMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;

    SECTION("contradictory mix stride withholds the concrete sink format without erasing roles")
    {
      mix.Format.nBlockAlign = 4;
      mix.Format.nAvgBytesPerSec = 192000;
      expectedMixMask = 3;
      expectedSinkMapping = ChannelDeliveryEvidence::Mapping::Mapped;
    }

    SECTION("an unavailable mix observation publishes no fabricated sink format")
    {
      mix.Format.nSamplesPerSec = 0;
      expectedMixRate = 0;
      expectedMixChannels = 0;
    }

    SECTION("reserved engine positions never become a sink layout")
    {
      mix.dwChannelMask = 0x40003;
      mixHasFormat = true;
      sinkHasFormat = true;
    }

    SECTION("wildcard engine positions never fabricate a conflicting role")
    {
      // A partial-keep regression would retain FL here and report a false
      // conflict with the client's BL/BR set; full erasure stays unknown.
      application.speakerMask = 0x30;
      mix.dwChannelMask = 0x80000001;
      mixHasFormat = true;
      sinkHasFormat = true;
    }

    auto const optClientWave = detail::waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = detail::observeWaveFormat(optClientWave->Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    auto const mixRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(mixRes);
    CHECK(mixRes->sampleRate == expectedMixRate);
    CHECK(mixRes->channels == expectedMixChannels);
    CHECK(mixRes->speakerMask == expectedMixMask);
    CHECK(mixRes->optFormat.has_value() == mixHasFormat);

    if (mixRes->optFormat)
    {
      CHECK(mixRes->optFormat->speakerMask == 0U);
    }

    auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optClientWave, *mixRes);
    CHECK(evidence.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK_FALSE(evidence.hasSharedConversion);

    auto registry = detail::WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .optMixFormat = mixRes->optFormat,
                      .mixChannels = mixRes->channels,
                      .mixSpeakerMask = mixRes->speakerMask,
                      .channelDelivery = evidence});

    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    CHECK(stream.channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == application.speakerMask);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.optFormat.has_value() == sinkHasFormat);
    CHECK(sink.channelDelivery.mapping == expectedSinkMapping);
    CHECK(sink.channelDelivery.nativeSpeakerMask == expectedMixMask);
    CHECK_FALSE(sink.channelDelivery.hasSharedConversion);

    if (sink.optFormat)
    {
      // A retained precision fact without roles is not a layout.
      CHECK(signalFormat(*sink.optFormat).speakerMask == 0U);
    }

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), signalFormat(application)));
    CHECK(analysis.channelConfidence == ChannelConfidence::Unverified);
    CHECK(analysis.fullyVerified == sinkHasFormat);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    REQUIRE(analysis.assessments.size() == 3U);
    CHECK(std::ranges::count(
            analysis.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    CHECK(std::ranges::count(
            analysis.assessments[2].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    CHECK(analysis.assessments[2].channelDelivery.nativeSpeakerMask == expectedMixMask);
  }

  TEST_CASE("observeWaveFormat leaves unsupported endpoint encoding unknown", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEX{};
    wave.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    wave.nChannels = 2;
    wave.nSamplesPerSec = 48000;
    wave.wBitsPerSample = 16;
    wave.nBlockAlign = 1;
    wave.nAvgBytesPerSec = 16000;

    auto const observationRes = detail::observeWaveFormat(wave);
    REQUIRE(observationRes);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == 0U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("observeWaveFormat keeps unknown subtype roles independent of PCM geometry",
            "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 1;
    wave.Format.nAvgBytesPerSec = 16000;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples = {.wValidBitsPerSample = 32};
    wave.SubFormat = {};
    wave.dwChannelMask = 0x3;
    // Unknown/non-PCM subtypes own their frame/block semantics. These literal
    // block and byte-rate values do not fit two-channel 32-bit PCM arithmetic;
    // only independent rate/count/roles survive, never a concrete PCM format.
    // Known PCM/float geometry failures remain covered by the contradictory-
    // arithmetic observation case, which also keeps independent facts.
    SECTION("non-PCM block semantics do not become a PCM geometry failure")
    {
    }

    SECTION("unknown container precision cannot produce a concrete format")
    {
      wave.Format.wBitsPerSample = 0;
    }

    SECTION("unknown subtype with zero valid precision still has no concrete format")
    {
      wave.Samples = {.wValidBitsPerSample = 0};
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == 0x3U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("wasapiInitializeFailureCause maps only the unsupported format to a route mismatch",
            "[audio][unit][wasapi][backend]")
  {
    struct CauseCase final
    {
      ::HRESULT status = S_OK;
      BackendOpenFailure::Cause cause = BackendOpenFailure::Cause::OutputFailure;
    };

    auto const causeCases =
      std::array{CauseCase{AUDCLNT_E_UNSUPPORTED_FORMAT, BackendOpenFailure::Cause::TrackRouteIncompatible},
                 CauseCase{E_INVALIDARG, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_OUTOFMEMORY, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_FAIL, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_ENDPOINT_CREATE_FAILED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_IN_USE, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_SERVICE_NOT_RUNNING, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_RESOURCES_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 // A failing status unknown to the Initialize policy stays terminal.
                 CauseCase{E_NOTIMPL, BackendOpenFailure::Cause::OutputFailure}};

    for (auto const& causeCase : causeCases)
    {
      CAPTURE(causeCase.status);
      CHECK(detail::wasapiInitializeFailureCause(causeCase.status) == causeCase.cause);
    }
  }
} // namespace ao::audio::backend::test
