// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/WasapiProvider.h"
#include "lib/audio/backend/WasapiSharedBackend.h"
#include "lib/audio/backend/detail/WasapiGraphRegistry.h"
#include "test/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ao::audio::backend::test
{
  TEST_CASE("WasapiSharedBackend - selected native endpoint observes declared roles and direct-out opens",
            "[audio][integration][wasapi][.manual]")
  {
    // Explicitly selected idle endpoint only. No start/render/PCM submission,
    // test setProperty call, endpoint-volume write or system-default change.
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
      bool isMapped;
      bool requiresSuccess;
    };
    // A declared multichannel set is offered with its exact roles; only a
    // zero-mask source above two channels stays a direct-out (mask zero)
    // open. The hidden campaign is an opt-in observation probe, not a
    // completion gate; its stereo control rows still require success when
    // the campaign is explicitly run.
    auto const requests = std::array{RequestCase{2, 0x3, 0x3, true, true},
                                     RequestCase{2, 0, 0x3, false, true},
                                     RequestCase{2, 0x30, 0x30, true, false},
                                     RequestCase{6, 0x3f, 0x3f, true, false},
                                     RequestCase{6, 0, 0, false, false}};

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
      CHECK(openedRes->channelDelivery.isMapped == request.isMapped);
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
      CHECK(stream->channelDelivery.isMapped == openedRes->channelDelivery.isMapped);
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
      CHECK(sink->channelDelivery.isMapped == (mix->speakerMask != 0U));

      if (mix->speakerMask != 0U)
      {
        CHECK(nativeMixMask == mix->speakerMask);
      }
      else
      {
        CHECK(std::popcount(nativeMixMask) < mix->channels);
      }

      // Literal registry unit cases own conversion-policy oracles. This dynamic
      // native control checks API/dataflow consistency, not independent policy
      // correctness or physical speaker delivery.
      bool const knownConversion =
        mix->channels != request.channels || (request.nativeMask != 0U && nativeMixMask != 0U &&
                                              std::popcount(request.nativeMask | nativeMixMask) > mix->channels);
      CHECK_FALSE(openedRes->channelDelivery.hasSharedConversion);
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
                       static_cast<std::uint32_t>(openedRes->channelDelivery.isMapped),
                       mix->sampleRate,
                       mix->channels,
                       sampleEncodingName(mix->encoding),
                       mix->speakerMask,
                       nativeMixMask,
                       static_cast<std::uint32_t>(sink->channelDelivery.isMapped),
                       sink->channelDelivery.hasSharedConversion));

      backend.close();
      REQUIRE_FALSE(received.empty());
      CHECK(received.back().nodes.empty());
    }
  }
} // namespace ao::audio::backend::test
