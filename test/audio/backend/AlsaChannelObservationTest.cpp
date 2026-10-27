// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"
#include "test/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - native stereo observations admit matching roles and refuse conflicting roles",
            "[.manual]")
  {
    // Explicitly selected, idle stereo endpoint with an independently known
    // current canonical, unflagged FL/FR order. This configures storage only:
    // no start, render, PCM frames, map SET, mixer write or system-default
    // change is performed.
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
    CHECK(openedRes->channelDelivery.isMapped);
    CHECK(openedRes->channelDelivery.nativeSpeakerMask == 0x3);
    CHECK_FALSE(openedRes->channelDelivery.hasSharedConversion);
    REQUIRE(openedRes->optEndpoint);
    CHECK(openedRes->optEndpoint->signalFormat.speakerMask == 0x3);
    CHECK(openedRes->optEndpoint->signalFormat.precisionBits >= 16);

    auto const identified = openedRes->clientFormat;
    REQUIRE(identified.sampleRate == 48000);
    auto unidentified = identified;
    unidentified.speakerMask = 0;
    Backend const& dispatch = backend;

    CHECK(backend.canReuseFormat(unidentified, identified));
    CHECK(backend.canReuseFormat(identified, unidentified));
    CHECK(dispatch.canReuseFormat(unidentified, identified));
    CHECK(dispatch.canReuseFormat(identified, unidentified));

    // These two client modes alias each other, but neither matches the
    // installed 48000 Hz stream whose channel-map evidence authorizes reuse.
    auto unrelatedUnidentified = unidentified;
    unrelatedUnidentified.sampleRate = 96000;
    auto unrelatedIdentified = unrelatedUnidentified;
    unrelatedIdentified.speakerMask = 0x3;
    CHECK_FALSE(backend.canReuseFormat(unrelatedUnidentified, unrelatedIdentified));
    CHECK_FALSE(backend.canReuseFormat(unrelatedIdentified, unrelatedUnidentified));

    backend.close();

    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
    CHECK(backend.canReuseFormat(identified, identified));

    // Same rate/count/precision, genuinely different speaker set. The native
    // current map, not the count or an Error code, establishes this constraint.
    source.speakerMask = 0x30;
    auto const conflictRes = backend.open(source, target);
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(conflictRes.error().error.code == Error::Code::FormatRejected);
    CHECK(conflictRes.error().error.message.contains("channel positions conflict"));

    backend.close();
  }
} // namespace ao::audio::backend::test
