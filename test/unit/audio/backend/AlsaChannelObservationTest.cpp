// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - native stereo observations admit matching roles and refuse conflicting roles",
            "[audio][integration][alsa][.manual]")
  {
    // Explicitly selected, idle stereo endpoint with an independently known
    // current FL/FR map. This configures storage only: no start, render, PCM
    // frames, map SET, mixer write or system-default change is performed.
    auto const* deviceId = std::getenv("AOBUS_TEST_ALSA_DEVICE_ID");

    if (deviceId == nullptr || *deviceId == '\0')
    {
      SKIP("Select an idle FL/FR hardware endpoint with AOBUS_TEST_ALSA_DEVICE_ID");
    }

    auto const device = Device{.id = DeviceId{std::string{deviceId}},
                               .displayName = "Selected native stereo observation endpoint",
                               .backendId = kBackendAlsa};
    auto target = audio::test::NoopRenderTarget{};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const openedRes = backend.open(source, target);
    INFO((openedRes ? "opened" : openedRes.error().error.message));
    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.speakerMask == 0x3);
    CHECK(openedRes->channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(openedRes->channelDelivery.nativeSpeakerMask == 0x3);
    CHECK_FALSE(openedRes->channelDelivery.hasSharedConversion);
    REQUIRE(openedRes->optEndpoint);
    CHECK(openedRes->optEndpoint->signalFormat.speakerMask == 0x3);
    CHECK(openedRes->optEndpoint->signalFormat.precisionBits >= 16);

    // Control-side reuse query on the actual admitted plans: both canonical
    // two-speaker identities share the installed identity transport here.
    auto const& configured = openedRes->clientFormat;
    auto const unpositioned =
      PcmFormat{.sampleRate = configured.sampleRate, .channels = 2, .encoding = configured.encoding, .speakerMask = 0};
    auto const backPair = PcmFormat{
      .sampleRate = configured.sampleRate, .channels = 2, .encoding = configured.encoding, .speakerMask = 0x30};
    CHECK(backend.canReusePcmMode(configured, configured));
    CHECK(backend.canReusePcmMode(configured, unpositioned));
    CHECK(backend.canReusePcmMode(unpositioned, configured));
    CHECK_FALSE(backend.canReusePcmMode(configured, backPair));
    backend.close();

    // Closing cleared the per-open proof before the lifetime change.
    CHECK_FALSE(backend.canReusePcmMode(configured, unpositioned));

    // Same rate/count/precision, genuinely different speaker set. The native
    // current map, not the count or an Error code, establishes this constraint.
    source.speakerMask = 0x30;
    auto const conflictRes = backend.open(source, target);
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(conflictRes.error().error.code == Error::Code::FormatRejected);
    CHECK(conflictRes.error().error.message.contains("channel positions conflict"));

    // The failed re-open reset the per-open proof: no nonexact reuse remains.
    CHECK_FALSE(backend.canReusePcmMode(configured, unpositioned));
    backend.close();
  }
} // namespace ao::audio::backend::test
